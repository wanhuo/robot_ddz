#include "EventProcess.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "message.pb.h"
#include "connect.pb.h"
#include "org_room2client.pb.h"
#include "stringutil.h"
#include "log.h"
#include "RobotConfig.h"
#include "confaccess.h"

using namespace std;
using namespace YLYQ;
using namespace Protocol;
using namespace message;
using namespace connect;
using namespace org_room2client;
using namespace robot;

EventProcess::EventProcess()
    :confAccess_(CConfAccess::GetConfInstance()),
     headerRobot_(0)
{
    InitParams();
    InitConnection(ip_, port_, robotNum_);
    InitRobot();
    InitTimer();
    InitSignal();
}

void EventProcess::Event(std::shared_ptr<Conn> conn, bool isErrorOccurence)
{
    if (conn->fd_ == headerRobot_)
    {
        headerRobot_ = 0;
        INFO("Header robot %d disconnected.", conn->robot_->GetRobotId());
    }
    else
    {
        INFO("Robot %d disconnected.", conn->robot_->GetRobotId());
    }
    if (isErrorOccurence)
    {
        INFO("Some error course connect break, and will try to reconnect.");
        ReConnect(this, conn->robot_->GetRobotId());
    }
}

void EventProcess::ReConnentEvent(std::shared_ptr<Conn> conn, int id)
{
    int fd = conn->fd_;
    connTypeIt findRet = mapConn_.find(fd);
    if (mapConn_.end() == findRet)
    {
        ERROR("Cannot found conn when reconnect server.");
        return;
    }
    findRet->second->robot_ = std::shared_ptr<Robot>(new Robot(id, robotIQLevel_));
    Verify(id, fd);
    INFO("Robot %d has reconnected to server.", id);
}


/*消息的格式
    +----------------+----------+----------------------------+
    +  len(Message)  |   msgId  |           Message          |
    +----------------+----------+-----------+----------------+
    +                           |   Head    |     body       |
    +----------------+----------+-----------+----------------+
    +    4 bytes     |  4 bytes |  12 bytes | len - 12 bytes |
    +----------------+---------------------------------------+
*/
void EventProcess::ReadEvent(std::shared_ptr<Conn> conn)
{
    //读取消息
    char msgLen[4] = {0};
    size_t len = 0;
    int iMsgLen = 0;
    int msgId = 0;
    int dataLength = conn->GetReadBufferLen();
    //DEBUG("Reveive data length is: %d.", dataLength);
    while (dataLength > 0)
    {
        //读取msg length
        memset(msgLen, '\0', 4);
        len = conn->GetReadBufferData(msgLen, 4);
        if (4 != len)
        {
            ERROR("Doesn't has 4 byte len info.");
            break;
        }
        int *pMsgLen = (int*)msgLen;
        iMsgLen = ntohl(*pMsgLen);
        if (0 == iMsgLen)
        {
            ERROR("Error! Convent data length failed.");
            break;
        }
        //DEBUG("Msg len: %d.", iMsgLen);

        //读取msgid
        memset(msgLen, '\0', 4);
        len = conn->GetReadBufferData(msgLen, 4);
        if (4 != len)
        {
            ERROR("Doesn't has 4 byte MsgId info.");
            break;
        }
        pMsgLen = (int*)msgLen;
        msgId = ntohl(*pMsgLen);
        //DEBUG("MsgId is: %d.", msgId);

        //读取消息体
        char* msg = new char[iMsgLen + 1];
        len = conn->GetReadBufferData(msg, iMsgLen);
        DEBUG("Receive %d byte from server for robot %d in message %d.", len, conn->robot_->GetRobotId(), msgId);

        //本次剩余未读字节
        dataLength = conn->GetReadBufferLen();
        DEBUG("Data still has length %d.", dataLength);

        string strMsg;
        strMsg.append(msg, iMsgLen);
        delete [] msg;
        string strRet;
        bool result = conn->robot_->RobotProcess(msgId, strMsg, strRet);
        if (result)
        {
            //需要发送数据
            SendMsg(msgId, strRet, conn);
        }
    }
    //DEBUG("Read data this time over.");
}

void EventProcess::SendMsg(int msgId, const string& strRet, shared_ptr<Conn> conn)
{
    int msgIdBak = msgId;
    switch (msgId)
    {
        case robot::MSGID_VERIFY_ACK: //身份验证请求应答, 接下来进行初始化游戏请求
            GameInit(conn->robot_->GetRobotId(), conn->fd_);
            return;
        case robot::NOTIFY_CALLSCORE:
            msgId = robot::MSGID_CALLSCORE_REQ; //叫分
            break;
        case robot::NOTIFY_DEALCARD:
            msgId = robot::MSGID_CALLSCORE_REQ; //叫分
            break;
        case robot::NOTIFY_TAKEOUT:
            msgId = robot::MSGID_TAKEOUT_REQ; //出牌
            break;
        case robot::NOTIFY_BASECARD:
            msgId = robot::MSGID_TAKEOUT_REQ; //出牌
            break;
        case robot::NOTIFY_TRUST:
            msgId = robot::MSGID_TRUST_CANCEL_REQ; //取消托管
            break;
        case robot::MSGID_KEEP_ACK:
            msgId = robot::MSGID_TRUST_CANCEL_REQ; //取消托管
            break;
        case robot::MSGID_INIT_GAME_ACK:
            msgId = robot::MSGID_KEEP_REQ; //断线续玩
            break;
        case robot::NOTIFY_SWITCH_SCENE:
            msgId = robot::MSGID_READY_REQ; //准备完毕，可以发牌
            break;
        case robot::MSGID_DDZ_SIGN_UP_CONDITION_ACK:
            if (HEADER == conn->robot_->GetStatus())
            {
                SendQueryRoomStatusReq(conn, false);
                return;
            }
            else
            {
                msgId = robot::MSGID_DDZ_SIGN_UP_REQ; //发送报名请求
            }
            break;
        case robot::MSGID_DDZ_ROOM_STAT_ACK:
            ChangeStatusForRobot(::atoi(strRet.c_str()));
            return;
        case robot::MSGID_DDZ_SIGN_UP_ACK://定时赛中可以发送报名请求
            ChangeStatusForRobot(::atoi(strRet.c_str()));
            return;
        case robot::MSGID_DDZ_QUICK_START_ACK://该机器人的金币不足，需要断开连接
            disConnect(conn);
            return;
    }

    string strSend;
    bool result = SerializeMsg(msgId, strRet, strSend);
    if (!result)
    {
        ERROR("Serialize message to be send failed.");
    }
    else
    {
        int robotId = conn->robot_->GetRobotId();
        if (robot::NOTIFY_DEALCARD == msgIdBak || robot::NOTIFY_BASECARD == msgIdBak)
        {
            //添加主动消息延时发送消息的定时器
            MsgNode* oneMsgNode = new MsgNode(delaySendActiveMsgTime_, 0, this, strSend, msgId, conn);
            addTimerEvent(delay_send_msg_time_cb, oneMsgNode, true);
            DEBUG("Add a timer, will delay send active message: %d to server for robot %d.", msgId, robotId);
        }
        else if (robot::NOTIFY_CALLSCORE == msgIdBak || robot::NOTIFY_TAKEOUT == msgIdBak)
        {
            //添加被动消息延时发送消息的定时器
            srand((int)time(NULL));
            int delayTime = delaySendPassiveMsgTime_ <= 1 ? 1 : (rand() % (delaySendPassiveMsgTime_ - 1) + 1);
            MsgNode* oneMsgNode = new MsgNode(delayTime, 0, this, strSend, msgId, conn);
            addTimerEvent(delay_send_msg_time_cb, oneMsgNode, true);
            DEBUG("Add a timer, will delay send passive message: %d to server for robot %d.", msgId, robotId);
        }
        else
        {
            //无需延迟，直接发送
            int iSendResult = conn->AddToWriteBuffer(strSend.c_str(), strSend.length());
            DEBUG("Send immediate message: %d to server for robot %d, send result: %d.", msgId, robotId, iSendResult);
        }
    }
}

void EventProcess::SendQueryRoomStatusReq(std::shared_ptr<Conn> conn, bool isTimer)
{
    if (HEADER != conn->robot_->GetStatus())
    {
        INFO("Robot %d isn't a header.", conn->robot_->GetRobotId());
        return;
    }
    INFO("Header robot\'s status is normal.");
    string strSend;
    if (isTimeTrial_ && isTimer)
    {
        //定时赛
        string serializedStr;
        OrgRoomDdzSignUpConditionReq orgRoomDdzSignUpConditionReq;
        orgRoomDdzSignUpConditionReq.set_matchid(matchId_);
        orgRoomDdzSignUpConditionReq.SerializeToString(&serializedStr);
        SerializeMsg(robot::MSGID_DDZ_SIGN_UP_CONDITION_REQ, serializedStr, strSend);
        INFO("Begin to query time trial %d status.", matchId_);
    }
    else
    {
        //游戏场和非定时赛
        string serializedStr;
        OrgRoomDdzRoomStatReq orgRoomDdzRoomStatReq;
        orgRoomDdzRoomStatReq.add_roomids(matchId_);
        orgRoomDdzRoomStatReq.SerializeToString(&serializedStr);
        SerializeMsg(robot::MSGID_DDZ_ROOM_STAT_REQ, serializedStr, strSend);
        INFO("Begin to query room %d status.", matchId_);
    }
    int iSendRtn = conn->AddToWriteBuffer(strSend.c_str(), strSend.length());
    DEBUG("Header robot %d send query requery once, send result is: %d.", conn->robot_->GetRobotId(), iSendRtn);
}

void EventProcess::ChangeStatusForRobot(int robotNum)
{
    INFO("Will set %d robot for %s.", robotNum, isMatch_ ? "match" : "game");

    //从队列中取出机器人，再将取出的机器人重新入队
    int queueSize = int(taskQueue_.size());
    while (queueSize-- && robotNum)
    {
        auto robotFd = taskQueue_.front();
        taskQueue_.pop();

        auto findRet = mapConn_.find(robotFd.first);//查找消息对应的机器人
        if ((mapConn_.end()) == findRet)
        {
            continue;//没找到
        }

        RobotStatus status = findRet->second->robot_->GetStatus();
        //找到
        if (WAITSIGNUP == status)
        {
            INFO("Choose robot %d.", findRet->second->robot_->GetRobotId());
            SendReqForRobot(findRet->second);
            robotFd.second = 1;
            --robotNum;
        }
        //else if (GAMMING != status && HEADER != status)
        //{
        //    DEBUG("This robot isn\'t avaluable, status is: %d.", status);
        //    robotFd.second += 1;
        //    if (robotFd.second > 2) { //当机器人处于该状态的次数大于2的时候，就主动改变其状态
        //        if (INITGAME == status || CANSINGUP == status || KEEPPLAY == status) {
        //            findRet->second->robot_->SetStatus(WAITSIGNUP);
        //            robotFd.second = 1;
        //            SendReqForRobot(findRet->second);
        //            --robotNum;
        //            DEBUG("robot %d status is %d, and changed to WAITSIGNUP.", findRet->second->robot_->GetRobotId(), status);
        //        }
        //    }
        //}
        taskQueue_.push(robotFd); //重新入队
    }

    if (0 == robotNum)
    {
        INFO("Find enough robot.");
    }
    else
    {
        INFO("Doesn't find enough robot, also need %d robot, waitting for next time.", robotNum);
    }
}

void EventProcess::SendReqForRobot(std::shared_ptr<Conn> conn)
{
    string strSend;
    if (isMatch_)
    {
        string serializedStr;
        OrgRoomDdzSignUpConditionReq orgRoomDdzSignUpConditionReq;
        orgRoomDdzSignUpConditionReq.set_matchid(matchId_);
        orgRoomDdzSignUpConditionReq.SerializeToString(&serializedStr);
        SerializeMsg(robot::MSGID_DDZ_SIGN_UP_CONDITION_REQ, serializedStr, strSend);
    }
    else
    {
        string serializedStr;
        OrgRoomDdzQuickStartReq orgRoomDdzQuickStartReq;
        orgRoomDdzQuickStartReq.set_roomid(matchId_);
        orgRoomDdzQuickStartReq.SerializeToString(&serializedStr);
        SerializeMsg(robot::MSGID_DDZ_QUICK_START_REQ, serializedStr, strSend);
    }
    conn->AddToWriteBuffer(strSend.c_str(), strSend.length());
    DEBUG("Robot %d send a request", conn->robot_->GetRobotId());
    conn->robot_->SetStatus(INITGAME);
}

void EventProcess::signal_cb(int signo, short event, void *arg)
{
    //message OrgRoomDdzCancelSignUpReq {
    //    required int32 matchId = 1;     // 比赛 ID
    //}
    DEBUG("Receved SIGINI signal, program will exit...");
    EventProcess* eventProcess = static_cast<EventProcess*>(arg);
    OrgRoomDdzCancelSignUpReq orgRoomDdzCancelSignUpReq;
    orgRoomDdzCancelSignUpReq.set_matchid(eventProcess->matchId_);
    string serializedStr;
    orgRoomDdzCancelSignUpReq.SerializeToString(&serializedStr);
    string strSend;
    eventProcess->SerializeMsg(robot::MSGID_DDZ_CANCEL_SIGN_UP_REQ, serializedStr, strSend);

    for (auto it : eventProcess->mapConn_)
    {
        if (SIGNUPED == it.second->robot_->GetStatus())
        {
            int iSendRtn = it.second->AddToWriteBuffer(strSend.c_str(), strSend.length());
            DEBUG("Send unsign req for robot %d.", it.second->robot_->GetRobotId());
            it.second->robot_->SetStatus(EXITTING);
        }
    }
    MsgNode* ExitTimer = new MsgNode(eventProcess->exitTime_);
    eventProcess->stop(ExitTimer);
}

void EventProcess::query_room_state_time_cb(int fd, short events, void* arg)
{
    INFO("***************** query_room_state_time_cb START ******************");
    MsgNode* oneMsgNode = static_cast<MsgNode*>(arg);
    EventProcess* eventProcess = dynamic_cast<EventProcess*>(oneMsgNode->tcpEventServer_);
    //message OrgRoomDdzRoomStatReq {
    //    repeated int32 roomIds = 1;     // 房间/比赛 ID 列表
    //}
    if (0 == eventProcess->headerRobot_)
    {
        //选定负责调度的那个机器人
        int index = 0;
        shared_ptr<Conn> conn;
        bool findHeaderRobot = false;
        for (auto it : eventProcess->mapConn_)
        {
            if (WAITSIGNUP == it.second->robot_->GetStatus())
            {
                findHeaderRobot = true;
                conn = it.second;
                break;
            }
        }
        if (!findHeaderRobot)
        {
            ERROR("Doesn't has valuable robot for header robot, waiting for next time.");
            return;
        }
        INFO("Choose robot %d as header robot.", conn->robot_->GetRobotId());
        eventProcess->headerRobot_ = conn->fd_;
        conn->robot_->SetStatus(HEADER);
    }
    shared_ptr<Conn> conn = eventProcess->mapConn_[eventProcess->headerRobot_];
    eventProcess->SendQueryRoomStatusReq(conn, true);
    INFO("Has %d robot in queue", eventProcess->taskQueue_.size());
}

void EventProcess::heart_beat_time_cb(int fd, short events, void* arg)
{
    MsgNode* oneMsgNode = static_cast<MsgNode*>(arg);
    EventProcess* eventProcess = dynamic_cast<EventProcess*>(oneMsgNode->tcpEventServer_);

    HeartbeatNtf heartbeatNtf;
    heartbeatNtf.set_rev("robot");  //暂且设为robot
    string serializedStr;
    heartbeatNtf.SerializeToString(&serializedStr);
    string strSend;
    eventProcess->SerializeMsg(robot::MSGID_HEARTBEAT_NTF, serializedStr, strSend);

    for (auto it : eventProcess->mapConn_)
    {
        it.second->AddToWriteBuffer(strSend.c_str(), strSend.length());
    }
    DEBUG("Send once heard beat.");
}

void EventProcess::Verify(int robotId, int fd)
{
    VerifyReq verifyReq;
    string strRobotId = StringUtil::Int2String(robotId);
    string sessionkey = confAccess_->GetSessionKey();
    verifyReq.set_userid(strRobotId);
    verifyReq.set_sessionkey(sessionkey + strRobotId);
    string serializedStr;
    verifyReq.SerializeToString(&serializedStr);
    string strSend;
    SerializeMsg(robot::MSGID_VERIFY_REQ, serializedStr, strSend);
    connTypeIt findRet = mapConn_.find(fd);
    if (mapConn_.end() == findRet)
    {
        ERROR("robot %d is not connected when verified.", robotId);
        return;
    }
    if (INIT == findRet->second->robot_->GetStatus())
    {
        findRet->second->AddToWriteBuffer(strSend.c_str(), strSend.length());
    }
    DEBUG("Robot %d send verify once.", robotId);
    taskQueue_.push(make_pair(fd, 1));
}

void EventProcess::GameInit(int robotId, int fd)
{
    string strType = confAccess_->GetGameType();
    string strName = confAccess_->GetGameName();
    INFO("type: %s, name: %s.", strType.c_str(), strName.c_str());

    InitGameReq initGameReq;
    initGameReq.set_type(strType);
    initGameReq.set_name(strName);
    string serializedStr;
    initGameReq.SerializeToString(&serializedStr);
    string strSend;
    serializedStr = SerializeMsg(robot::MSGID_INIT_GAME_REQ, serializedStr, strSend);

    connTypeIt findRet = mapConn_.find(fd);
    if (mapConn_.end() == findRet)
    {
        ERROR("robot %d is not connected when game init.", robotId);
        return;
    }

    if (VERIFIED == findRet->second->robot_->GetStatus())
    {
        findRet->second->AddToWriteBuffer(strSend.c_str(), strSend.length());
        DEBUG("Robot %d send init game once.", robotId);
    }
}

void EventProcess::delay_send_msg_time_cb(int fd, short events, void* arg)
{
    MsgNode* oneMsgNode = static_cast<MsgNode*>(arg);
    EventProcess* eventProcess = dynamic_cast<EventProcess*>(oneMsgNode->tcpEventServer_);
    int iSendResult = oneMsgNode->conn_->AddToWriteBuffer(oneMsgNode->msg_.c_str(), oneMsgNode->msg_.length());
    DEBUG("Send delay message: %d to server for robot %d, write %d size, send result is %d.", \
            oneMsgNode->msgId_, oneMsgNode->conn_->robot_->GetRobotId(), oneMsgNode->msg_.length(), iSendResult);
    eventProcess->delTimerEvent(oneMsgNode);
}

bool EventProcess::SerializeMsg( int msgId, const string& body, string& strRet )
{
    Message message;
    message.set_body( body );
    message.mutable_head()->set_version(1);     //暂且设为1
    message.mutable_head()->set_sequence(1);    //暂且设为1
    message.mutable_head()->set_timestamp(1);   //暂且设为1
    string serializedStr;
    if (!message.SerializeToString(&serializedStr))
    {
        ERROR("Serialized protobuf msg failed.");
        return false;
    }

    if (!message.IsInitialized())
    {
        ERROR("Isn't a legal protobuf packet.");
        return false;
    }

    int msgLen = int(serializedStr.length());
    msgLen = htonl(msgLen);
    msgId = htonl(msgId);

    strRet.append((char*)&msgLen, sizeof(msgLen));
    strRet.append((char*)&msgId, sizeof(msgId));
    strRet.append(serializedStr.c_str(), (int)serializedStr.length());
    return true;
}

void EventProcess::InitRobot()
{
    DEBUG("***************** InitRobot START ******************");
    int index = 0;
    for (auto it : mapConn_)
    {
        int robotId = robotIdStart_ + index;
        it.second->robot_ = std::shared_ptr<Robot>(new Robot(robotId, robotIQLevel_));
        INFO("Robot %d inited, send verified req", robotIdStart_ + index - 1);
        Verify(robotId, it.second->fd_);
        ++index;
    }
}

void EventProcess::InitSignal()
{
    DEBUG("***************** InitSignal START ******************");
    addSignalEvent(signal_cb, SIGINT);
}

void EventProcess::InitTimer()
{
    DEBUG("***************** InitTimer START ******************");
    HeartBeatTimer = new MsgNode(heartBeatTime_, 0, this);
    QueryRoomStateTimer = new MsgNode(roomStateTime_, 0, this);
    //+++++++++++++++++++++++++++//
    addTimerEvent(heart_beat_time_cb, HeartBeatTimer, false);
    addTimerEvent(query_room_state_time_cb, QueryRoomStateTimer, false);
}

void EventProcess::InitParams()
{
    DEBUG("***************** InitParams START ******************");
    ip_ = confAccess_->GetIP();
    port_ = confAccess_->GetPort();
    robotNum_ = confAccess_->GetRobotNum();
    robotIQLevel_ = confAccess_->GetIQLevel();
    robotIdStart_ = confAccess_->GetRobotIdRangeStart();
    robotIdEnd_ = confAccess_->GetRobotIdRangeEnd();
    heartBeatTime_ = confAccess_->GetHeartBeatTime();
    verifyTime_ = confAccess_->GetVerifyTime();
    initGameTime_ = confAccess_->GetInitGameTime();
    delaySendActiveMsgTime_ = confAccess_->GetSendActiveMsgDelayTime();
    delaySendPassiveMsgTime_ = confAccess_->GetSendPassiveMsgDelayTime();
    exitTime_ = confAccess_->GetProgramExitTime();
    roomStateTime_ = confAccess_->GetQueryRoomStateTime();
    matchId_ = confAccess_->GetMatchId();
    isMatch_ = confAccess_->GetIsMatch();
    isTimeTrial_ = confAccess_->GetIsTimeTrial();

    if (!CheckConnCount())
    {
        ::exit(1);
    }
}

bool EventProcess::CheckConnCount()
{
    if (robotIdStart_ < 0 || robotIdEnd_ < 0 || robotNum_ < 0)
    {
        ERROR("robotid or robot num small than zero.");
        return false;
    }

    if (robotIdEnd_ < robotIdStart_)
    {
        ERROR("robot id range is wrong.");
        return false;
    }

    if ((robotIdEnd_ - robotIdStart_) < robotNum_)
    {
        robotNum_ = robotIdEnd_ - robotIdStart_;
        INFO("robotNum is changed to %d.", robotNum_);
    }
    return true;
}
