#include "simple_message/socket/tcp_server.h"
//轨迹(多个轨迹点)
#include "simple_message/joint_traj.h"
//轨迹点及对应消息
#include "simple_message/joint_traj_pt.h"
#include "simple_message/messages/joint_traj_pt_message.h"
//机器人状态及对应消息
#include "simple_message/messages/robot_status_message.h"
//关节位置和对应消息
#include "simple_message/messages/joint_message.h"
//#include "simple_message/simple_comms_fault_handler.h"

using namespace industrial;

#include <future>
#include <thread> //多线程
#include <atomic>
#include <chrono>

#define PORT_RECEIVE 8080 //端口号
#define PORT_SEND 8081 //端口号
const int MAX_TRAJ_LENGTH=200; //最多200个轨迹点
const int SENSOR_TIME=100;   //传感器更新周期
const int EXECUTE_TIME=150; //执行时长



std::vector<joint_data::JointData> recTrajBuffer(MAX_TRAJ_LENGTH);//定义的轨迹点数组
std::vector<joint_data::JointData> execTrajBuffer(MAX_TRAJ_LENGTH);//定义的轨迹点数组

std::atomic<bool>  trajLockFlag(false);
std::atomic<bool>  stopMotionFlag(false);
std::atomic<bool>  sensorStatusFlag(true);
joint_data::JointData   sensorData;
shared_types::shared_int       buffer_size=0;//缓存的轨迹点的个数
shared_types::shared_int       traj_size=0;//执行的轨迹点的个数



//将缓存的轨迹数组复制到执行线程
inline void activateExcuteMotion()
{
    //等待trajLock解锁
    while(trajLockFlag.load())
    {
        std::this_thread::yield();
    }
    trajLockFlag.store(true);
    //将缓存中的数据复制到执行数组
    traj_size=buffer_size;

    //traj_size大于0的时候才复制
    if(traj_size>0)
    {
        execTrajBuffer.swap(recTrajBuffer);
    }
    //解锁
    trajLockFlag.store(false);
}


//将接收到的轨迹点存入轨迹缓存数组
void addJointData2Buffer(joint_data::JointData &jdata)
{
    if(buffer_size>=MAX_TRAJ_LENGTH)
    {
        LOG_WARN("Too Many Trajectory Points,Trajectory has already reached its maximum size");
    }
    else
    {
        recTrajBuffer[buffer_size]=jdata;
        ++buffer_size;
    }
}


void threadReceiveTrajectory()
{
    const int tcpPort = PORT_RECEIVE;
    char ipAddr[] = "127.0.0.1";
    // 构建server
    LOG_INFO("Motion Server Start Initialize");
    tcp_server::TcpServer tcpServer;
    if (!tcpServer.init(tcpPort))
    {
        LOG_INFO("Motion Server Initialize Failed!");
        return;
    }

    while (!tcpServer.makeConnect())
    {
        LOG_INFO("Motion Server No Client Connect");
        std::this_thread::yield();
    }
    LOG_INFO("Motion Server Client Connected");
    simple_message::SimpleMessage msgRecv,msgSend;        //接收到的二进制数据

    joint_traj_pt_message::JointTrajPtMessage trajPtMsg; //接收到的轨迹点消息
    joint_data::JointData jointData;              //关节位置/速度数据


    shared_types::shared_int SEQ_TYPE;
    while (true)
    {
        if(tcpServer.receiveMsg(msgRecv))
        {
            trajPtMsg.init(msgRecv);
            SEQ_TYPE=trajPtMsg.point_.getSequence();
            trajPtMsg.point_.getJointPosition(jointData);
            switch (SEQ_TYPE)
            {
            case joint_traj_pt::SpecialSeqValues::STOP_TRAJECTORY:
                buffer_size=0;
                stopMotionFlag.store(true);
                activateExcuteMotion();//停止执行
                break;
            case joint_traj_pt::SpecialSeqValues::START_TRAJECTORY_DOWNLOAD:
                buffer_size=0;
                stopMotionFlag.store(false);//取消停止运动的标示
                addJointData2Buffer(jointData);//添加轨迹点
                break;
            case joint_traj_pt::SpecialSeqValues::END_TRAJECTORY:
                addJointData2Buffer(jointData);//添加轨迹点
                activateExcuteMotion();//执行程序
                break;
            default:
                addJointData2Buffer(jointData);//添加轨迹点
                break;
            }

            //发送成功接收轨迹点的响应:默认的请求模式是CommTypes::INVALID
            if(trajPtMsg.getCommType()==simple_message::CommTypes::SERVICE_REQUEST)
            {
                //发送接收成功的消息
                msgSend.init(simple_message::StandardMsgTypes::JOINT_TRAJ_PT,
                             simple_message::CommTypes::SERVICE_REPLY,
                             simple_message::ReplyTypes::SUCCESS);
                tcpServer.sendMsg(msgSend);
            }
        }
    }
}

//线程:执行轨迹
void threadExecuteMotion()
{
    int index=0;
    while(true)
    {
        //当轨迹被读取锁定
//        if(trajLockFlag.load())
//        {
//            continue;
//        }
        //上锁
        trajLockFlag.store(true);
        //执行轨迹
        if(traj_size>0)
        {

            LOG_INFO("Execute Trajectory...");
            for(index=0;index<traj_size;++index)
            {
                //如果有暂停信号
                if(stopMotionFlag.load())
                {
                    break;
                }
//                LOG_INFO("[ExecutePosition] %10.8f %10.8f %10.8f %10.8f %10.8f %10.8f %10.8f",
//                         execTrajBuffer[index].getJoint(0),
//                         execTrajBuffer[index].getJoint(1),
//                         execTrajBuffer[index].getJoint(2),
//                         execTrajBuffer[index].getJoint(3),
//                         execTrajBuffer[index].getJoint(4),
//                         execTrajBuffer[index].getJoint(5),
//                         execTrajBuffer[index].getJoint(6));//EXCUTE_MOTION伪代码

                sensorStatusFlag.store(false);
                sensorData.copyFrom(execTrajBuffer[index]);
                std::this_thread::sleep_for(std::chrono::milliseconds(EXECUTE_TIME));//执行
                sensorStatusFlag.store(true);
            }
        }
        traj_size=0;//清除
        trajLockFlag.store(false);//解锁
    }
    ////错误处理伪代码
    //catch(ERROR)
    //{
        //HANDLE_ERROR();
    //}
}
void threadRobotStatusReply()
{

    const int tcpPort = PORT_SEND;
    char ipAddr[] = "127.0.0.1";
    // 构建server
    LOG_INFO("Status Server Start Initialize");
    tcp_server::TcpServer tcpServer;
    if (!tcpServer.init(tcpPort))
    {
        LOG_INFO("Status Server Initialize Failed!");
        return;
    }

    while (!tcpServer.makeConnect())
    {
        LOG_DEBUG("Status Server No Client Connect");
        std::this_thread::yield();
    }
    LOG_INFO("Status Server Client Connected");
    simple_message::SimpleMessage msgSend;        //发送的的二进制数据

    joint_message::JointMessage  jointMsg;       //发送给上位机的关节位置
    robot_status::RobotStatus robotStatus; //发送给上位机的机器人的状态
    robot_status_message::RobotStatusMessage robotStatusMsg;
    //drivesPowered:驱动上电
    //eStopped:eStopped
    //errorCode:自定义标示(和机器人相关)
    //inError:机器人发生故障
    //inMotion:机器人正在运动
    //mode:机器人的模式(UNKNOWN/MANUAL/AUTO)
    //motionPossible:能否运动
    robotStatus.init(robot_status::TriStates::TS_TRUE,
                     robot_status::TriStates::TS_FALSE,
                     123,
                     robot_status::TriStates::TS_FALSE,
                     robot_status::TriStates::TS_FALSE,
                     robot_status::RobotModes::AUTO,
                     robot_status::TriStates::TS_TRUE);
    robotStatusMsg.init(robotStatus);

    joint_data::JointData data;
    data.init();
    while (true)
    {
//        if(sensorStatusFlag.load())
//        {
            data.copyFrom(sensorData);
            jointMsg.init(0,data);
//        }
        jointMsg.toTopic(msgSend);
        tcpServer.sendMsg(msgSend);


        robotStatusMsg.toTopic(msgSend);
        tcpServer.sendMsg(msgSend);

        LOG_INFO("Sensor Robot Position and Status...");
        std::this_thread::sleep_for(std::chrono::milliseconds(SENSOR_TIME));
    }
}


int main(int argc, char **argv)
{
    auto threadReceive=std::async(std::launch::async,threadReceiveTrajectory);
    auto threadStatus=std::async(std::launch::async,threadRobotStatusReply);
    auto threadMoton=std::async(std::launch::async,threadExecuteMotion);

}
