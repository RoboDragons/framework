/***************************************************************************
 *   Copyright 2020 Michael Eischer, Philipp Nordhus, Andreas Wendler      *
 *   Robotics Erlangen e.V.                                                *
 *   http://www.robotics-erlangen.de/                                      *
 *   info@robotics-erlangen.de                                             *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   any later version.                                                    *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "simulator.h"
#include "core/rng.h"
#include "core/timer.h"
#include "core/coordinates.h"
#include "protobuf/ssl_wrapper.pb.h"
#include "protobuf/geometry.h"
#include "simball.h"
#include "simfield.h"
#include "simrobot.h"
#include "erroraggregator.h"
#include <QTimer>
#include <algorithm>
#include <QtDebug>
#include <QVector>
#include <cstdint>

using namespace camun::simulator;

/* Friction and restitution between robots, ball and field: (empirical measurments)
 * Ball vs. Robot:
 * Restitution: about 0.60
 * Friction: trial and error in simulator 0.18 (similar results as in reality)
 *
 * Ball vs. Floor:
 * Restitution: sqrt(h'/h) = sqrt(0.314) = 0.56
 * Friction: \mu_k = -a / g (while slipping) = 0.35
 *
 * Robot vs. Floor:
 * Restitution and Friction should be as low as possible
 *
 * Calculations:
 * Variables: r: restitution, f: friction
 * Indices: b: ball; f: floor; r: robot
 *
 * r_b * r_f = 0.56
 * r_b * r_r = 0.60
 * r_f * r_r = small
 * => r_b = 1; r_f = 0.56; r_r = 0.60
 *
 * f_b * f_f = 0.35
 * f_b * f_r = 0.22
 * f_f * f_r = very small
 * => f_b = 1; f_f = 0.35; f_r = 0.22
 */

struct camun::simulator::SimulatorData
{
    RNG rng;
    btDefaultCollisionConfiguration *collision;
    btCollisionDispatcher *dispatcher;
    btBroadphaseInterface *overlappingPairCache;
    btSequentialImpulseConstraintSolver *solver;
    btDiscreteDynamicsWorld *dynamicsWorld;
    world::Geometry geometry;
    QVector<SSL_GeometryCameraCalibration> reportedCameraSetup;
    QVector<btVector3> cameraPositions;
    SimField *field;
    SimBall *ball;
    Simulator::RobotMap robotsBlue;
    Simulator::RobotMap robotsYellow;
    QMap<uint32_t, robot::Specs> specsBlue;
    QMap<uint32_t, robot::Specs> specsYellow;
    bool flip;
    float stddevBall;
    float stddevBallArea;
    float stddevRobot;
    float stddevRobotPhi;
    float ballDetectionsAtDribbler; // per robot per second
    bool enableInvisibleBall;
    float ballVisibilityThreshold;
    float cameraOverlap;
    float cameraPositionError;
    float objectPositionOffset;
    float robotCommandPacketLoss;
    float robotReplyPacketLoss;
    float missingBallDetections;
    bool dribblePerfect;
    float missingRobotDetections;
    uint64_t commandDelay;
};

static void simulatorTickCallback(btDynamicsWorld *world, btScalar timeStep)
{
    Simulator *sim = reinterpret_cast<Simulator *>(world->getWorldUserInfo());
    sim->handleSimulatorTick(timeStep);
}

/*!
 * \class Simulator
 * \ingroup simulator
 * \brief %Simulator interface
 */

Simulator::Simulator(const Timer *timer, const amun::SimulatorSetup &setup, bool useManualTrigger) :
    m_isPartial(useManualTrigger),
    m_timer(timer),
    m_time(0),
    m_lastSentStatusTime(0),
    m_timeScaling(1.),
    m_enabled(false),
    m_charge(false),
    m_visionDelay(35 * 1000 * 1000),
    m_visionProcessingTime(5 * 1000 * 1000),
    m_aggregator(new ErrorAggregator(this))
{
    // triggers by default every 5 milliseconds if simulator is enabled
    // timing may change if time is scaled
    m_trigger = new QTimer(this);
    m_trigger->setTimerType(Qt::PreciseTimer);
    if (!m_isPartial) {
        connect(m_trigger, SIGNAL(timeout()), SLOT(process()));
    }

    // setup bullet
    m_data = new SimulatorData;
    m_data->collision = new btDefaultCollisionConfiguration();
    m_data->dispatcher = new btCollisionDispatcher(m_data->collision);
    m_data->overlappingPairCache = new btDbvtBroadphase();
    m_data->solver = new btSequentialImpulseConstraintSolver;
    m_data->dynamicsWorld = new btDiscreteDynamicsWorld(m_data->dispatcher, m_data->overlappingPairCache, m_data->solver, m_data->collision);
    m_data->dynamicsWorld->setGravity(btVector3(0.0f, 0.0f, -9.81f * SIMULATOR_SCALE));
    m_data->dynamicsWorld->setInternalTickCallback(simulatorTickCallback, this, true);

    m_data->geometry.CopyFrom(setup.geometry());
    for (const auto& camera : setup.camera_setup()) {
        m_data->reportedCameraSetup.append(camera);
        Vector visionPosition(camera.derived_camera_world_tx(), camera.derived_camera_world_ty());
        btVector3 truePosition;
        coordinates::fromVision(visionPosition, truePosition);
        truePosition.setZ(camera.derived_camera_world_tz() / 1000.0f);
        m_data->cameraPositions.append(truePosition);
    }

    // add field and ball
    m_data->field = new SimField(m_data->dynamicsWorld, m_data->geometry);
    m_data->ball = new SimBall(&m_data->rng, m_data->dynamicsWorld);
    connect(m_data->ball, &SimBall::sendSSLSimError, m_aggregator, &ErrorAggregator::aggregate);
    m_data->flip = false;
    m_data->stddevBall = 0.0f;
    m_data->stddevBallArea = 0.0f;
    m_data->stddevRobot = 0.0f;
    m_data->stddevRobotPhi = 0.0f;
    m_data->ballDetectionsAtDribbler = 0.0f;
    m_data->enableInvisibleBall = true;
    m_data->ballVisibilityThreshold = 0.4;
    m_data->cameraOverlap = 0.3;
    m_data->cameraPositionError = 0;
    m_data->objectPositionOffset = 0;
    m_data->robotCommandPacketLoss = 0;
    m_data->robotReplyPacketLoss = 0;
    m_data->missingBallDetections = 0;
    m_data->dribblePerfect = false;
    m_data->missingRobotDetections = 0;
    m_data->commandDelay = 0;

    // no robots after initialisation

    connect(timer, &Timer::scalingChanged, this, &Simulator::setScaling);
}

// does delete all Simrobots in the RobotMap, does not clear map
// (just like qDeleteAll would)
static void deleteAll(const Simulator::RobotMap& map) {
    for(const auto& e : map) {
        delete e.first;
    }
}

Simulator::~Simulator()
{
    resetVisionPackets();

    deleteAll(m_data->robotsBlue);
    deleteAll(m_data->robotsYellow);
    delete m_data->ball;
    delete m_data->field;
    delete m_data->dynamicsWorld;
    delete m_data->solver;
    delete m_data->overlappingPairCache;
    delete m_data->dispatcher;
    delete m_data->collision;
    delete m_data;
}

void Simulator::process()
{
    Q_ASSERT(m_time != 0);
    const qint64 start_time = Timer::systemTime();

    const qint64 current_time = m_timer->currentTime();

    // first: send vision packets in partial mode
    if (m_isPartial) {
        while(m_visionPackets.size() > 0 && std::get<2>(m_visionPackets.head()) >= current_time) {
            sendVisionPacket();
        }
    }

    // collect responses from robots
    QList<robot::RadioResponse> responses;

    // apply only radio commands that were already received by the robots
    while (m_radioCommands.size() > 0 && std::get<1>(m_radioCommands.head()) + m_data->commandDelay < m_time) {
        RadioCommand commands = m_radioCommands.dequeue();
        for (const sslsim::RobotCommand& command : std::get<0>(commands)->robot_commands()) {

            if (m_data->robotCommandPacketLoss > 0 && m_data->rng.uniformFloat(0, 1) <= m_data->robotCommandPacketLoss) {
                continue;
            }

            // pass radio command to robot that matches the id
            const auto id = command.id();
            SimulatorData* data = m_data;
            auto time = m_time;
            auto charge = m_charge;
            auto fabricateResponse = [data, &responses, time, charge, &id, &command](const Simulator::RobotMap& map, const bool* isBlue) {
                if (!map.contains(id)) return;
                robot::RadioResponse response = map[id].first->setCommand(command, data->ball, charge,
                                                                                   data->robotCommandPacketLoss, data->robotReplyPacketLoss);
                response.set_time(time);

                if (isBlue != nullptr) {
                    response.set_is_blue(*isBlue);
                }
                // only collect valid responses
                if (response.IsInitialized()) {
                    if (data->robotReplyPacketLoss == 0 || data->rng.uniformFloat(0, 1) > data->robotReplyPacketLoss) {
                        responses.append(response);
                    }
                }
            };
            bool blue = true;
            if (std::get<2>(commands)) {
                fabricateResponse(m_data->robotsBlue, &blue);
            } else {
                blue = false;
                fabricateResponse(m_data->robotsYellow, &blue);
            }
        }
    }

    // radio responses are sent when a robot gets his command
    // thus send the responses immediatelly
    emit sendRadioResponses(responses);
    sendSSLSimErrorInternal(ErrorSource::BLUE);
    sendSSLSimErrorInternal(ErrorSource::YELLOW);
    sendSSLSimErrorInternal(ErrorSource::CONFIG);

    // simulate to current strategy time
    double timeDelta = (current_time - m_time) * 1E-9;
    m_data->dynamicsWorld->stepSimulation(timeDelta, 10, SUB_TIMESTEP);
    m_time = current_time;

    // only send a vision packet every third frame = 15 ms - epsilon (=half frame)
    // gives a vision frequency of 66.67Hz
    if (m_lastSentStatusTime + 12500000 <= m_time) {
        auto data = createVisionPacket();



        if (m_isPartial) {
            std::get<2>(data) = m_time + m_visionDelay;
            m_visionPackets.enqueue(data);
        } else {
            m_visionPackets.enqueue(data);
            // timeout is in milliseconds
            int timeout = m_visionDelay * 1E-6 / m_timeScaling;

            // send after timeout, default timer mode, may jitter a bit
            QTimer *timer = new QTimer();
            timer->setTimerType(Qt::PreciseTimer);
            timer->setSingleShot(true);
            connect(timer, SIGNAL(timeout()), SLOT(sendVisionPacket()));
            timer->start(timeout);
            m_visionTimers.enqueue(timer);
        }

        m_lastSentStatusTime = m_time;
    }

    // send timing information
    Status status(new amun::Status);
    status->mutable_timing()->set_simulator((Timer::systemTime() - start_time) * 1E-9f);
    emit sendStatus(status);
}

void Simulator::sendSSLSimErrorInternal(ErrorSource source)
{
    QList<SSLSimError> errors = m_aggregator->getAggregates(source);
    if (errors.size() == 0) return;
    emit sendSSLSimError(errors, source);
}

static void createRobot(Simulator::RobotMap &list, float x, float y, uint32_t id, const ErrorAggregator* agg, SimulatorData* data, const QMap<uint32_t, robot::Specs>& teamSpecs)
{
    SimRobot *robot = new SimRobot(&data->rng, teamSpecs[id], data->dynamicsWorld, btVector3(x, y, 0), 0.f);
    robot->setDribbleMode(data->dribblePerfect);
    robot->connect(robot, &SimRobot::sendSSLSimError, agg, &ErrorAggregator::aggregate);
    list[id] = {robot, teamSpecs[id].generation()};

}

void Simulator::resetFlipped(Simulator::RobotMap &robots, float side)
{
    // find flipped robots and align them on a line
    const float x = m_data->geometry.field_width() / 2 - 0.2;
    float y = m_data->geometry.field_height() / 2 - 0.2;

    for (RobotMap::iterator it = robots.begin(); it != robots.end(); ++it) {
        SimRobot *robot = it.value().first;
        if (robot->isFlipped()) {
            SimRobot *new_robot = new SimRobot(&m_data->rng, robot->specs(), m_data->dynamicsWorld, btVector3(x, side * y, 0), 0.0f);
            delete robot;
            connect(new_robot, &SimRobot::sendSSLSimError, m_aggregator, &ErrorAggregator::aggregate); // TODO? use createRobot instead of this. However, doing so naively will break the iteration, so I left it for now.
            new_robot->setDribbleMode(m_data->dribblePerfect);
            it.value().first = new_robot;
        }
        y -= 0.3;
    }
}

void Simulator::handleSimulatorTick(double timeStep)
{
    // has to be done according to bullet wiki
    m_data->dynamicsWorld->clearForces();

    resetFlipped(m_data->robotsBlue, 1.0f);
    resetFlipped(m_data->robotsYellow, -1.0f);
    if (m_data->ball->isInvalid()) {
        delete m_data->ball;
        m_data->ball = new SimBall(&m_data->rng, m_data->dynamicsWorld);
        connect(m_data->ball, &SimBall::sendSSLSimError, m_aggregator, &ErrorAggregator::aggregate);
    }

    // apply commands and forces to ball and robots
    m_data->ball->begin();
    for(const auto& pair : m_data->robotsBlue) {
        pair.first->begin(m_data->ball, timeStep);
    }
    for(const auto& pair : m_data->robotsYellow) {
        pair.first->begin(m_data->ball, timeStep);
    }

    // add gravity to all ACTIVE objects
    // thus has to be done after applying commands
    m_data->dynamicsWorld->applyGravity();
}

static bool checkCameraID(const int cameraId, const btVector3 &p, const QVector<btVector3> &cameraPositions, const float overlap)
{
    float minDistance = std::numeric_limits<float>::max();
    float ownDistance = 0;
    for (int i = 0;i<cameraPositions.size();i++) {
        // manhattan distance for rectangular camera regions (if the cameras are distributed normally)
        float distance = std::abs(cameraPositions[i].x() - p.x()) + std::abs(cameraPositions[i].y() - p.y());
        minDistance = std::min(minDistance, distance);
        if (i == cameraId) {
            ownDistance = distance;
        }
    }
    return ownDistance <= minDistance + 2 * overlap;
}

void Simulator::initializeDetection(SSL_DetectionFrame *detection, std::size_t cameraId)
{
    detection->set_frame_number(m_lastFrameNumber[cameraId]++);
    detection->set_camera_id(cameraId);
    detection->set_t_capture((m_time + m_visionDelay - m_visionProcessingTime)*1E-9);
    detection->set_t_sent((m_time + m_visionDelay)*1E-9);
}

static btVector3 positionOffsetForCamera(float offsetStrength, btVector3 cameraPos)
{
    btVector3 cam2d{cameraPos.x(), cameraPos.y(), 0};
    if (offsetStrength < 1e-9) {
        // do not produce an offset that tiny
        return {0, 0, 0};
    }
    if (cam2d.length() < offsetStrength ) {
        // do not normalize a 0 vector
        return cam2d;
    }
    return btVector3(cameraPos.x(), cameraPos.y(), 0).normalized() * offsetStrength;
}

std::tuple<QList<QByteArray>, QByteArray, qint64> Simulator::createVisionPacket()
{
    const std::size_t numCameras = m_data->reportedCameraSetup.size();
    world::SimulatorState simState;
    simState.set_time(m_time);

    std::vector<SSL_DetectionFrame> detections(numCameras);
    for (std::size_t i = 0;i<numCameras;i++) {
        initializeDetection(&detections[i], i);
    }

    auto* ball = simState.mutable_ball();
    m_data->ball->writeBallState(ball);

    const btVector3 ballPosition = m_data->ball->position() / SIMULATOR_SCALE;
    if (m_time - m_lastBallSendTime >= m_minBallDetectionTime) {
        m_lastBallSendTime = m_time;

        for (std::size_t cameraId = 0; cameraId < numCameras; ++cameraId) {
            // at least one id is always valid
            if (!checkCameraID(cameraId, ballPosition, m_data->cameraPositions, m_data->cameraOverlap)) {
                continue;
            }

            bool missingBall = m_data->missingBallDetections > 0 && m_data->rng.uniformFloat(0, 1) <= m_data->missingBallDetections;
            if (missingBall) {
                continue;
            }

            // get ball position
            const btVector3 positionOffset = positionOffsetForCamera(m_data->objectPositionOffset, m_data->cameraPositions[cameraId]);
            bool visible = m_data->ball->update(detections[cameraId].add_balls(), m_data->stddevBall, m_data->stddevBallArea, m_data->cameraPositions[cameraId],
                    m_data->enableInvisibleBall, m_data->ballVisibilityThreshold, positionOffset);
            if (!visible) {
                detections[cameraId].clear_balls();
            }
        }
    }

    // get robot positions
    for (bool teamIsBlue : {true, false}) {
        auto &team = teamIsBlue ? m_data->robotsBlue : m_data->robotsYellow;

        for (const auto& it : team) {
            SimRobot* robot = it.first;
            auto* robotProto = teamIsBlue ? simState.add_blue_robots() : simState.add_yellow_robots();
            robot->update(robotProto, m_data->ball);

            if (m_time - robot->getLastSendTime() >= m_minRobotDetectionTime) {
                const float timeDiff = (m_time - robot->getLastSendTime()) * 1E-9;
                const btVector3 robotPos = robot->position() / SIMULATOR_SCALE;

                for (std::size_t cameraId = 0; cameraId < numCameras; ++cameraId) {

                    if (!checkCameraID(cameraId, robotPos, m_data->cameraPositions, m_data->cameraOverlap)) {
                        continue;
                    }

                    bool missingRobot = m_data->missingRobotDetections > 0 && m_data->rng.uniformFloat(0, 1) <= m_data->missingRobotDetections;
                    if (missingRobot) {
                        continue;
                    }

                    const btVector3 positionOffset = positionOffsetForCamera(m_data->objectPositionOffset, m_data->cameraPositions[cameraId]);
                    if (teamIsBlue) {
                        robot->update(detections[cameraId].add_robots_blue(), m_data->stddevRobot, m_data->stddevRobotPhi, m_time, positionOffset);
                    } else {
                        robot->update(detections[cameraId].add_robots_yellow(), m_data->stddevRobot, m_data->stddevRobotPhi, m_time, positionOffset);
                    }

                    // once in a while, add a ball mis-detection at a corner of the dribbler
                    // in real games, this happens because the ball detection light beam used by many teams is red
                    float detectionProb = timeDiff * m_data->ballDetectionsAtDribbler;
                    if (m_data->ballDetectionsAtDribbler > 0 && m_data->rng.uniformFloat(0, 1) < detectionProb) {
                        // always on the right side of the dribbler for now
                        if (!m_data->ball->addDetection(detections[cameraId].add_balls(), robot->dribblerCorner(false) / SIMULATOR_SCALE,
                                                        m_data->stddevRobot, 0, m_data->cameraPositions[cameraId], false, 0, positionOffset)) {
                            detections[cameraId].mutable_balls()->DeleteSubrange(detections[cameraId].balls_size()-1, 1);
                        }
                    }
                }
            }
        }
    }

    std::vector<SSL_WrapperPacket> packets;
    packets.reserve(numCameras);

    // add a wrapper packet for all detections (also for empty ones).
    // The reason is that other teams might rely on the fact that these detections are in regular intervals.
    for (auto &frame : detections) {

        // if multiple balls are reported, shuffle them randomly (the tracking might have systematic errors depending on the ball order)
        if (frame.balls_size() > 1) {
            std::shuffle(frame.mutable_balls()->begin(), frame.mutable_balls()->end(), rand_shuffle_src);
        }

        SSL_WrapperPacket packet;
        packet.mutable_detection()->CopyFrom(frame);
        packets.push_back(packet);
    }

    // add field geometry
    if (packets.size() == 0) {
        packets.push_back(SSL_WrapperPacket());
    }
    SSL_GeometryData *geometry = packets[0].mutable_geometry();
    SSL_GeometryFieldSize *field = geometry->mutable_field();
    convertToSSlGeometry(m_data->geometry, field);

    const btVector3 positionErrorSimScale = btVector3(0.3f, 0.7f, 0.05f).normalized() * m_data->cameraPositionError;
    btVector3 positionErrorVisionScale{0, 0, positionErrorSimScale.z() * 1000};
    coordinates::toVision(positionErrorSimScale, positionErrorVisionScale);
    for (const auto &calibration : m_data->reportedCameraSetup) {
        auto calib = geometry->add_calib();
        calib->CopyFrom(calibration);
        calib->set_derived_camera_world_tx(calib->derived_camera_world_tx() + positionErrorVisionScale.x());
        calib->set_derived_camera_world_ty(calib->derived_camera_world_ty() + positionErrorVisionScale.y());
        calib->set_derived_camera_world_tz(calib->derived_camera_world_tz() + positionErrorVisionScale.z());
    }

    // add ball model to geometry data
    geometry->mutable_models()->mutable_straight_two_phase()->set_acc_roll(-0.35);
    geometry->mutable_models()->mutable_straight_two_phase()->set_acc_slide(-3.9);
    geometry->mutable_models()->mutable_straight_two_phase()->set_k_switch(0.69);
    geometry->mutable_models()->mutable_chip_fixed_loss()->set_damping_z(0.566);
    geometry->mutable_models()->mutable_chip_fixed_loss()->set_damping_xy_first_hop(0.715);
    geometry->mutable_models()->mutable_chip_fixed_loss()->set_damping_xy_other_hops(1);

    // serialize "vision packet"
    QList<QByteArray> data;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        QByteArray d;
        d.resize(packets[i].ByteSize());
        if (packets[i].SerializeToArray(d.data(), d.size())) {
            data.push_back(d);
        } else {
            data.push_back(QByteArray());
        }
    }

    QByteArray d;
    d.resize(simState.ByteSize());
    if (!simState.SerializeToArray(d.data(), d.size())) {
        d = {};
    }
    return {data,d, 0};
}

void Simulator::sendVisionPacket()
{
    auto currentVisionPackets = m_visionPackets.dequeue();
    for (const QByteArray &data : std::get<0>(currentVisionPackets)) {
        emit gotPacket(data, m_timer->currentTime(), "simulator"); // send "vision packet" and assume instant receiving
        // the receive time may be a bit jittered just like a real transmission

    }
    emit sendRealData(std::get<1>(currentVisionPackets));
    if (!m_isPartial) {
        QTimer *timer = m_visionTimers.dequeue();
        timer->deleteLater();
    }
}

void Simulator::resetVisionPackets()
{
    qDeleteAll(m_visionTimers);
    m_visionTimers.clear();
    m_visionPackets.clear();
}

void Simulator::handleRadioCommands(const SSLSimRobotControl &commands, bool isBlue, qint64 processingStart)
{
    m_radioCommands.enqueue(std::make_tuple(commands, processingStart, isBlue));
}


void Simulator::setTeam(Simulator::RobotMap &list, float side, const robot::Team &team, QMap<uint32_t, robot::Specs>& teamSpecs)
{
    // remove old team
    deleteAll(list);
    list.clear();

    // changing a team is also triggering a tracking reset
    // thus the old robots will disappear immediatelly
    // however if the delayed vision packets arrive the old robots will be tracked again
    // thus after removing a robot from a team it can take 1 simulated second for the robot to disappear
    // to prevent this remove outdated vision packets
    resetVisionPackets();

    // align robots on a line
    const float x = m_data->geometry.field_width() / 2 - 0.2;
    float y = m_data->geometry.field_height() / 2 - 0.2;


    for (int i = 0; i < team.robot_size(); i++) {
        const robot::Specs& specs = team.robot(i);
        const auto id = specs.id();

        // (color, robot id) must be unique
        if (list.contains(id)) {
            std::cerr << "Error: Two ids for the same color, aborting!" << std::endl;
            continue;
        }
        teamSpecs[id].CopyFrom(specs);



        createRobot(list, x, side * y, id, m_aggregator, m_data, teamSpecs);
        y -= 0.3;
    }
}


#define FLIP(X, ATTR) do{if(X.has_##ATTR()){X.set_##ATTR(-X.ATTR());}} while(0)

void Simulator::moveBall(const sslsim::TeleportBall& ball)
{
    // remove the dribbling constraint
    if (!ball.has_by_force() || !ball.by_force()) {
        for (const auto& robotList : {m_data->robotsBlue, m_data->robotsYellow}) {
            for (const auto& it : robotList) {
                it.first->stopDribbling();
            }
        }
    }

    sslsim::TeleportBall b = ball;
    if (m_data->flip) {
        FLIP(b, x);
        FLIP(b, y);
        FLIP(b, vx);
        FLIP(b, vy);
    }

    if (b.teleport_safely()) {
        if (!b.has_x() || !b.has_y()) {
            SSLSimError error{new sslsim::SimulatorError};
            error->set_code("TELEPORT_SAFELY_PARTIAL");
            error->set_message("teleporting the ball safly with partial coordinates is not possible");
            m_aggregator->aggregate(error, ErrorSource::CONFIG);
            return;
        }
        safelyTeleportBall(b.x(), b.y());
    }

    m_data->ball->move(b);

}

void Simulator::moveRobot(const sslsim::TeleportRobot &robot) {
    if (!robot.id().has_team()) return;
    if (!robot.id().has_id()) return;
    bool is_blue = robot.id().team() == gameController::Team::BLUE;

    RobotMap& list = is_blue ? m_data->robotsBlue : m_data->robotsYellow;
    bool isPresent = list.contains(robot.id().id());
    QMap<uint32_t, robot::Specs>& teamSpecs = is_blue ? m_data->specsBlue : m_data->specsYellow;
    if (robot.has_present()) {
        if (robot.present() && !isPresent) {
            // add the requested robot
            if (!teamSpecs.contains(robot.id().id())) {
                SSLSimError error{new sslsim::SimulatorError};
                error->set_code("CREATE_UNSPEC_ROBOT");
                std::string message = "trying to create robot " + std::to_string(robot.id().id());
                message += ", but no spec for this robot was found";
                error->set_message(std::move(message));
                m_aggregator->aggregate(error, ErrorSource::CONFIG);
            } else if(!robot.has_x() || !robot.has_y()){
                SSLSimError error{new sslsim::SimulatorError};
                error->set_code("CREATE_NOPOS_ROBOT");
                std::string message = "trying to create robot " + std::to_string(robot.id().id());
                message += " without giving a position";
                error->set_message(std::move(message));
                m_aggregator->aggregate(error, ErrorSource::CONFIG);
            } else {
                Vector targetPos;
                coordinates::fromVision(robot, targetPos);
                //TODO: check if the given position is fine
                createRobot(list, targetPos.x, targetPos.y, robot.id().id(), m_aggregator, m_data, teamSpecs);
            }
        }
        else if (!robot.present() && isPresent) {
            //remove the robot
            auto val = list.take(robot.id().id());
            val.first->stopDribbling();
            delete val.first;
            return;
        }
        else if (!robot.present() && !isPresent) {
            return;
        }
        // Fall though: If the robot is already on the field and needs to be on the field, we just use that robot.
    } else {
        if (!isPresent) return;
    }

    if (!list.contains(robot.id().id())) return; // Recheck the list in case the has_present paragraph did change it.


    sslsim::TeleportRobot r = robot;

    if (m_data->flip) {
        FLIP(r, x);
        FLIP(r, y);
        FLIP(r, v_x);
        FLIP(r, v_y);
    }

    SimRobot* sim_robot = list[robot.id().id()].first;
    if (!r.has_by_force() || !r.by_force()) {
        sim_robot->stopDribbling();
    }
    sim_robot->move(r);
}

void Simulator::setFlipped(bool flipped)
{
    m_data->flip = flipped;
}

void Simulator::handleCommand(const Command &command)
{
    bool teamOrPerfectDribbleChanged = false;

    if (command->has_simulator()) {
        const amun::CommandSimulator &sim = command->simulator();
        if (sim.has_enable()) {
            m_enabled = sim.enable();
            m_time = m_timer->currentTime();
            // update timer when simulator status is changed
            setScaling(m_timeScaling);
        }

        if (sim.has_realism_config()) {
            auto realism = sim.realism_config();
            if (realism.has_stddev_ball_p()) {
                m_data->stddevBall = realism.stddev_ball_p();
            }

            if (realism.has_stddev_robot_p()) {
                m_data->stddevRobot = realism.stddev_robot_p();
            }

            if (realism.has_stddev_robot_phi()) {
                m_data->stddevRobotPhi = realism.stddev_robot_phi();
            }

            if (realism.has_stddev_ball_area()) {
                m_data->stddevBallArea = realism.stddev_ball_area();
            }

            if (realism.has_dribbler_ball_detections()) {
                m_data->ballDetectionsAtDribbler = realism.dribbler_ball_detections();
            }

            if (realism.has_enable_invisible_ball()) {
                m_data->enableInvisibleBall = realism.enable_invisible_ball();
            }

            if (realism.has_ball_visibility_threshold()) {
                m_data->ballVisibilityThreshold = realism.ball_visibility_threshold();
            }

            if (realism.has_camera_overlap()) {
                m_data->cameraOverlap = realism.camera_overlap();
            }

            if (realism.has_camera_position_error()) {
                m_data->cameraPositionError = realism.camera_position_error();
            }

            if (realism.has_object_position_offset()) {
                m_data->objectPositionOffset = realism.object_position_offset();
            }

            if (realism.has_robot_command_loss()) {
                m_data->robotCommandPacketLoss = realism.robot_command_loss();
            }

            if (realism.has_robot_response_loss()) {
                m_data->robotReplyPacketLoss = realism.robot_response_loss();
            }

            if (realism.has_missing_ball_detections()) {
                m_data->missingBallDetections = realism.missing_ball_detections();
            }

            if (realism.has_missing_robot_detections()) {
                m_data->missingRobotDetections = realism.missing_robot_detections();
            }

            if (realism.has_vision_delay()) {
                m_visionDelay = std::max((qint64)0, (qint64)realism.vision_delay());
            }

            if (realism.has_vision_processing_time()) {
                m_visionProcessingTime = std::max((qint64)0, (qint64)realism.vision_processing_time());
            }

            if (realism.has_simulate_dribbling()) {
                m_data->dribblePerfect = !realism.simulate_dribbling();
                teamOrPerfectDribbleChanged = true;
            }

            if (realism.has_command_delay()) {
                m_data->commandDelay = realism.command_delay();
            }
        }

        if (sim.has_ssl_control()) {
            const auto& sslControl = sim.ssl_control();
            if (sslControl.has_teleport_ball()) {
                moveBall(sslControl.teleport_ball());
            }
            for (const auto& moveR : sslControl.teleport_robot()) {
                moveRobot(moveR);
            }
        }

        if (sim.has_vision_worst_case()) {
            if (sim.vision_worst_case().has_min_ball_detection_time()) {
                m_minBallDetectionTime = sim.vision_worst_case().min_ball_detection_time() * 1E9;
            }
            if (sim.vision_worst_case().has_min_robot_detection_time()) {
                m_minRobotDetectionTime = sim.vision_worst_case().min_robot_detection_time() * 1E9;
            }
        }

        if (sim.has_set_simulator_state()) {
            if (sim.set_simulator_state().has_ball()) {
                m_data->ball->restoreState(sim.set_simulator_state().ball());
            }
            const auto restoreRobots = [](RobotMap& map, auto robots) {
                for(const auto& robot: robots) {
                    if (map.contains(robot.id())) {
                        map[robot.id()].first->restoreState(robot);
                    }
                }
            };
            restoreRobots(m_data->robotsYellow, sim.set_simulator_state().yellow_robots());
            restoreRobots(m_data->robotsBlue, sim.set_simulator_state().blue_robots());
        }
    }

    if (command->has_transceiver()) {
        const amun::CommandTransceiver &t = command->transceiver();
        if (t.has_charge()) {
            m_charge = t.charge();
        }
    }

    if (command->has_set_team_blue()) {
        teamOrPerfectDribbleChanged = true;
        setTeam(m_data->robotsBlue, 1.0f, command->set_team_blue(), m_data->specsBlue);
    }

    if (command->has_set_team_yellow()) {
        teamOrPerfectDribbleChanged = true;
        setTeam(m_data->robotsYellow, -1.0f, command->set_team_yellow(), m_data->specsYellow);
    }

    if (teamOrPerfectDribbleChanged) {
        for (const auto& robotList : {m_data->robotsBlue, m_data->robotsYellow}) {
            for (const auto& it : robotList) {
                SimRobot *robot = it.first;
                robot->setDribbleMode(m_data->dribblePerfect);
            }
        }
    }
}

void Simulator::setScaling(double scaling)
{
    if (scaling <= 0 || !m_enabled) {
        m_trigger->stop();
        // clear pending vision packets
        resetVisionPackets();
    } else {
        // scale default timing of 5 milliseconds
        const int t = 5 / scaling;
        m_trigger->start(qMax(1, t));

        // The vision packet timings are wrong after a scaling change
        // In addition if the new scaling is larger than the old one,
        // this would cause the timers started after the scaling change
        // to trigger before the old timers, what causes the deletion of
        // the old timers before they are fired.
        resetVisionPackets();
    }
    // needed if scaling is set before simulator was enabled
    m_timeScaling = scaling;
}

void Simulator::seedPRGN(uint32_t seed)
{
    m_data->rng.seed(seed);
}

static bool overlapCheck(const btVector3& p0, const float& r0, const btVector3& p1, const float& r1)
{
    const float distance = (p1 - p0).length();
    return distance <= r0+r1;
}

// uses the real world scale
void Simulator::teleportRobotToFreePosition(SimRobot *robot)
{
    btVector3 robotPos = robot->position() / SIMULATOR_SCALE;
    btVector3 direction = (robotPos - m_data->ball->position() / SIMULATOR_SCALE).normalize();
    float distance = 2 * (BALL_RADIUS + robot->specs().radius());
    bool valid = true;
    do {
        valid = true;
        robotPos = robotPos + 2 * direction*distance;

        for (const auto& robotList : {m_data->robotsBlue, m_data->robotsYellow}) {
            for (const auto& it : robotList) {
                SimRobot *robot2 = it.first;
                if (robot == robot2) {
                    continue;
                }

                btVector3 tmp = robot2->position() / SIMULATOR_SCALE;
                if (overlapCheck(robotPos, robot->specs().radius(), tmp, robot2->specs().radius())) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                break;
            }
        }
    } while(!valid);

    sslsim::TeleportRobot robotCommand;
    robotCommand.mutable_id()->set_id(robot->specs().id());
    coordinates::toVision(robotPos, robotCommand);

    robotCommand.set_v_x(0);
    robotCommand.set_v_y(0);
    robot->move(robotCommand);
}

void Simulator::safelyTeleportBall(const float x, const float y)
{
    // remove the speed of all robots in this radius to avoid them running over the ball
    const float STOP_ROBOTS_RADIUS = 1.5f;

    btVector3 newBallPos(x, y, 0);
    for (const auto& robotList : {m_data->robotsBlue, m_data->robotsYellow}) {
        for (const auto& it : robotList) {
            SimRobot* robot = it.first;
            btVector3 robotPos = robot->position() / SIMULATOR_SCALE;
            if (overlapCheck(newBallPos, BALL_RADIUS, robotPos, robot->specs().radius())) {
                teleportRobotToFreePosition(robot);
            } else if (overlapCheck(newBallPos, STOP_ROBOTS_RADIUS, robotPos, robot->specs().radius())) {
                // set the speed to zero but keep the robot where it is
                sslsim::TeleportRobot robotCommand;
                robotCommand.mutable_id()->set_id(robot->specs().id());
                robotCommand.set_v_x(0);
                robotCommand.set_v_y(0);
                robot->move(robotCommand);
            }
        }
    }
}
