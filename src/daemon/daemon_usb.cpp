/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "daemon_usb.h"
#include "usb_ffs.h"

namespace Hdc {
HdcDaemonUSB::HdcDaemonUSB(const bool serverOrDaemonIn, void *ptrMainBase)
    : HdcUSBBase(serverOrDaemonIn, ptrMainBase)
{
    usbMain = nullptr;
    Base::ZeroStruct(sendEP);
    uv_mutex_init(&sendEP);
}

HdcDaemonUSB::~HdcDaemonUSB()
{
    // Closed in the IO loop, no longer closing CLOSEENDPOINT
    uv_mutex_destroy(&sendEP);
}

void HdcDaemonUSB::Stop()
{
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop");
    // Here only clean up the IO-related resources, session related resources clear reason to clean up the session
    // module
    modRunning = false;
    if (!usbMain) {
        return;
    }
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop free main session");
    HdcDaemon *daemon = (HdcDaemon *)clsMainBase;
    Base::TryCloseHandle((uv_handle_t *)&checkEP);
    daemon->FreeSession(usbMain->sessionId);
    if (usbMain->hUSB != nullptr) {
        CloseEndpoint(usbMain->hUSB);
    }
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB Stop free main session finish");
    usbMain = nullptr;
    // workaround for sendEP mutex only
}

int HdcDaemonUSB::Initial()
{
    // 4.4   Kit Kat      |19, 20     |3.10
    // after Linux-3.8，kernel switch to the USB Function FS
    // Implement USB hdc function in user space
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB init");
    if (access(USB_FFS_HDC_EP0, F_OK) != 0) {
        WRITE_LOG(LOG_DEBUG, "Just support usb-ffs, must kernel3.8+ and enable usb-ffs, usbmod disable");
        return -1;
    }
    const uint16_t usbFfsScanInterval = 3000;
    HdcDaemon *daemon = (HdcDaemon *)clsMainBase;
    usbMain = daemon->MallocSession(false, CONN_USB, this);
    if (!usbMain) {
        WRITE_LOG(LOG_DEBUG, "CheckNewUSBDeviceThread malloc failed");
        return -1;
    }
    WRITE_LOG(LOG_DEBUG, "HdcDaemonUSB::Initiall");
    uv_timer_init(&daemon->loopMain, &checkEP);
    checkEP.data = this;
    uv_timer_start(&checkEP, WatchEPTimer, 0, usbFfsScanInterval);
    return 0;
}

// DAEMON end USB module USB-FFS EP port connection
int HdcDaemonUSB::ConnectEPPoint(HUSB hUSB)
{
    int ret = -1;
    while (true) {
        if (!hUSB->control) {
            // After the control port sends the instruction, the device is initialized by the device to the HOST host,
            // which can be found for USB devices. Do not send initialization to the EP0 control port, the USB
            // device will not be initialized by Host
            WRITE_LOG(LOG_DEBUG, "enter ConnectEPPoint");
            WRITE_LOG(LOG_DEBUG, "Begin send to control(EP0) for usb descriptor init");
            if ((hUSB->control = open(USB_FFS_HDC_EP0, O_RDWR)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: cannot open control endpoint: errno=%d", USB_FFS_HDC_EP0, errno);
                break;
            }
            if (write(hUSB->control, &USB_FFS_DESC, sizeof(USB_FFS_DESC)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: write ffs_descriptors failed: errno=%d", USB_FFS_HDC_EP0, errno);
                break;
            }
            if (write(hUSB->control, &USB_FFS_VALUE, sizeof(USB_FFS_VALUE)) < 0) {
                WRITE_LOG(LOG_WARN, "%s: write USB_FFS_VALUE failed: errno=%d", USB_FFS_HDC_EP0, errno);
                break;
            }
            // active usbrc，Send USB initialization singal
            Base::SetHdcProperty("sys.usb.ffs.ready", "1");
            WRITE_LOG(LOG_DEBUG, "ConnectEPPoint ctrl init finish, set usb-ffs ready");
        }
        if ((hUSB->bulkOut = open(USB_FFS_HDC_OUT, O_RDWR)) < 0) {
            WRITE_LOG(LOG_WARN, "%s: cannot open bulk-out ep: errno=%d", USB_FFS_HDC_OUT, errno);
            break;
        }
        if ((hUSB->bulkIn = open(USB_FFS_HDC_IN, O_RDWR)) < 0) {
            WRITE_LOG(LOG_WARN, "%s: cannot open bulk-in ep: errno=%d", USB_FFS_HDC_IN, errno);
            break;
        }
        int flags = fcntl(hUSB->bulkIn, F_GETFL, 0);
        fcntl(hUSB->bulkIn, flags | O_NONBLOCK);
        ret = 0;
        break;
    }
    if (ret < 0) {
        CloseEndpoint(hUSB);
    }
    return ret;
}

void HdcDaemonUSB::CloseEndpoint(HUSB hUSB)
{
    if (!isAlive) {
        return;
    }
    if (hUSB->bulkIn > 0) {
        close(hUSB->bulkIn);
        hUSB->bulkIn = 0;
    }
    if (hUSB->bulkOut > 0) {
        close(hUSB->bulkOut);
        hUSB->bulkOut = 0;
    }
    if (hUSB->control > 0) {
        close(hUSB->control);
        hUSB->control = 0;
    }
    isAlive = false;
    WRITE_LOG(LOG_FATAL, "DaemonUSB CloseEndpoint");
}

// Prevent other USB data misfortunes to send the program crash
bool HdcDaemonUSB::AvailablePacket(uint8_t *ioBuf, uint32_t *sessionId)
{
    bool ret = false;
    constexpr auto maxBufFactor = 1.2;
    while (true) {
        struct USBHead *usbPayloadHeader = (struct USBHead *)ioBuf;
        if (memcmp(usbPayloadHeader->flag, PACKET_FLAG.c_str(), PACKET_FLAG.size())) {
            break;
        }
        if (usbPayloadHeader->dataSize > MAX_SIZE_IOBUF * maxBufFactor + sizeof(USBHead)) {
            break;
        }
        if ((usbPayloadHeader->option & USB_OPTION_RESET)) {
            HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
            // The Host end program is restarted, but the USB cable is still connected
            WRITE_LOG(LOG_WARN, "Hostside want restart daemon, restart old sessionid:%d", usbPayloadHeader->sessionId);
            daemon->PushAsyncMessage(usbPayloadHeader->sessionId, ASYNC_FREE_SESSION, nullptr, 0);
            break;
        }
        *sessionId = usbPayloadHeader->sessionId;
        ret = true;
        break;
    }
    return ret;
}

// Work in subcrete，Work thread is ready
bool HdcDaemonUSB::ReadyForWorkThread(HSession hSession)
{
    HdcUSBBase::ReadyForWorkThread(hSession);
    return true;
};

// daemon, usb-ffs data sends a critical function
// The speed of sending is too fast, IO will cause memory stacking, temporarily do not use asynchronous
int HdcDaemonUSB::SendUSBIOSync(HSession hSession, HUSB hMainUSB, uint8_t *data, const int length)
{
    int bulkIn = hMainUSB->bulkIn;
    int childRet = 0;
    int ret = -1;
    int offset = 0;
    if (!isAlive) {
        goto Finish;
    }
    if (!modRunning) {
        goto Finish;
    }
    while (modRunning && !hSession->isDead) {
        childRet = write(bulkIn, (uint8_t *)data + offset, length - offset);
        if (childRet <= 0) {
            int err = errno;
            if (err == EINTR) {
                WRITE_LOG(LOG_DEBUG, "BulkinWrite write EINTR, try again");
                continue;
            } else {
                WRITE_LOG(LOG_DEBUG, "BulkinWrite write fatal errno %d", err);
            }
            break;
        }
        offset += childRet;
        if (offset >= length) {
            break;
        }
    }
    if (offset == length) {
        ret = length;
    } else {
        WRITE_LOG(LOG_FATAL, "BulkinWrite write failed, nsize:%d really:%d", length, offset);
    }
Finish:
    USBHead *pUSBHead = (USBHead *)data;
    if (pUSBHead->option & USB_OPTION_TAIL) {
        hSession->sendRef--;
    }
    if (ret < 0 && isAlive) {
        WRITE_LOG(LOG_FATAL, "BulkinWrite CloseEndpoint");
        // It actually closed the subsession, the EP port is also closed
        CloseEndpoint(hMainUSB);
    }
    return ret;
}

int HdcDaemonUSB::SendUSBRaw(HSession hSession, uint8_t *data, const int length)
{
    HdcDaemon *daemon = (HdcDaemon *)hSession->classInstance;
    // Prevent memory stacking, send temporary way to use asynchronous
    // Generally sent in the same thread, but when new session is created, there is a possibility that the old session
    // is not retired.
    // At present, the radical transmission method is currently opened directly in various threads, and
    // it can be used exclusive File-DESC transmission mode in each thread. The late stage can be used as asynchronous +
    // SendPipe to the main thread transmission.
    uv_mutex_lock(&sendEP);
    int ret = SendUSBIOSync(hSession, usbMain->hUSB, data, length);
    if (ret < 0) {
        daemon->FreeSession(hSession->sessionId);
        WRITE_LOG(LOG_DEBUG, "SendUSBRaw try to freesession");
    }
    uv_mutex_unlock(&sendEP);
    return ret;
}

// cross thread call
void HdcDaemonUSB::OnNewHandshakeOK(const uint32_t sessionId)
{
    currentSessionId = sessionId;  // real Id
}

HSession HdcDaemonUSB::PrepareNewSession(uint32_t sessionId, uint8_t *pRecvBuf, int recvBytesIO)
{
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    // new session
    HSession hChildSession = daemon->MallocSession(false, CONN_USB, this, sessionId);
    if (!hChildSession) {
        return nullptr;
    }
    if (currentSessionId != 0) {
        // reset old session
        // The Host side is restarted, but the USB cable is still connected
        WRITE_LOG(LOG_WARN, "New session coming, restart old sessionid:%d", currentSessionId);
        daemon->PushAsyncMessage(currentSessionId, ASYNC_FREE_SESSION, nullptr, 0);
    }
    Base::StartWorkThread(&daemon->loopMain, daemon->SessionWorkThread, Base::FinishWorkThread, hChildSession);
    auto funcNewSessionUp = [](uv_timer_t *handle) -> void {
        HSession hChildSession = reinterpret_cast<HSession>(handle->data);
        HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(hChildSession->classInstance);
        if (hChildSession->childLoop.active_handles == 0) {
            return;
        }
        if (!hChildSession->isDead) {
            auto ctrl = daemon->BuildCtrlString(SP_START_SESSION, 0, nullptr, 0);
            Base::SendToStream((uv_stream_t *)&hChildSession->ctrlPipe[STREAM_MAIN], ctrl.data(), ctrl.size());
            WRITE_LOG(LOG_DEBUG, "Main thread usbio mirgate finish");
        }
        Base::TryCloseHandle(reinterpret_cast<uv_handle_t *>(handle), Base::CloseTimerCallback);
    };
    Base::TimerUvTask(&daemon->loopMain, hChildSession, funcNewSessionUp);
    return hChildSession;
}

int HdcDaemonUSB::DispatchToWorkThread(HSession hSession, const uint32_t sessionId, uint8_t *readBuf, int readBytes)
{
    // Format:USBPacket1 payload1...USBPacketn
    // payloadn-[USBHead1(PayloadHead1+Payload1)]+[USBHead2(Payload2)]+...+[USBHeadN(PayloadN)]
    HSession hChildSession = nullptr;
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(hSession->classInstance);
    hChildSession = daemon->AdminSession(OP_QUERY, sessionId, nullptr);
    if (!hChildSession) {
        hChildSession = PrepareNewSession(sessionId, readBuf, readBytes);
        if (!hChildSession) {
            return ERR_SESSION_NOFOUND;
        }
    }
    if (!SendToHdcStream(hChildSession, reinterpret_cast<uv_stream_t *>(&hChildSession->dataPipe[STREAM_MAIN]),
        readBuf, readBytes)) {
        return ERR_IO_FAIL;
    }
    return readBytes;
}

bool HdcDaemonUSB::JumpAntiquePacket(const uint8_t &buf, ssize_t bytes) const
{
    constexpr size_t antiqueFlagSize = 4;
    constexpr size_t antiqueFullSize = 24;
    // anti CNXN 0x4e584e43
    uint8_t flag[] = { 0x43, 0x4e, 0x58, 0x4e };
    if (bytes == antiqueFullSize && !memcmp(&buf, flag, antiqueFlagSize)) {
        return true;
    }
    return false;
}

// Only physically swap EP ports will be reset
void HdcDaemonUSB::OnUSBRead(uv_fs_t *req)
{  // Only read at the main thread
    auto ctxIo = reinterpret_cast<CtxUvFileCommonIo *>(req->data);
    auto hSession = reinterpret_cast<HSession>(ctxIo->data);
    auto thisClass = reinterpret_cast<HdcDaemonUSB *>(ctxIo->thisClass);
    uint8_t *bufPtr = ctxIo->buf;
    ssize_t bytesIOBytes = req->result;
    uint32_t sessionId = 0;
    bool ret = false;
    while (true) {
        // Don't care is module running, first deal with this
        if (bytesIOBytes < 0) {
            WRITE_LOG(LOG_WARN, "USBIO failed1 %s", uv_strerror(bytesIOBytes));
            break;
        }
        if (thisClass->JumpAntiquePacket(*bufPtr, bytesIOBytes)) {
            WRITE_LOG(LOG_DEBUG, "JumpAntiquePacket auto jump");
            ret = true;
            break;
        }
        // guess is head of packet
        if (!thisClass->AvailablePacket((uint8_t *)bufPtr, &sessionId)) {
            WRITE_LOG(LOG_WARN, "AvailablePacket check failed, ret:%d buf:%-50s", bytesIOBytes, bufPtr);
            break;
        }
        if (thisClass->DispatchToWorkThread(hSession, sessionId, bufPtr, bytesIOBytes) < 0) {
            WRITE_LOG(LOG_FATAL, "DispatchToWorkThread failed");
            break;
        }
        if (thisClass->LoopUSBRead(hSession) < 0) {
            WRITE_LOG(LOG_FATAL, "LoopUSBRead failed");
            break;
        }
        ret = true;
        break;
    }
    delete[] ctxIo->buf;
    uv_fs_req_cleanup(req);
    delete ctxIo;
    if (!ret || !thisClass->modRunning) {
        thisClass->CloseEndpoint(hSession->hUSB);
    }
}

int HdcDaemonUSB::LoopUSBRead(HSession hSession)
{
    int ret = -1;
    HUSB hUSB = hSession->hUSB;
    HdcDaemon *daemon = reinterpret_cast<HdcDaemon *>(clsMainBase);
    // must > available size, or it will be incorrect
    int readMax = Base::GetMaxBufSize() + sizeof(USBHead) + EXTRA_ALLOC_SIZE;
    auto ctxIo = new CtxUvFileCommonIo();
    auto buf = new uint8_t[readMax]();
    uv_fs_t *req = nullptr;
    uv_buf_t iov;
    if (ctxIo == nullptr || buf == nullptr) {
        goto FAILED;
    }
    ctxIo->buf = buf;
    ctxIo->bufSize = readMax;
    ctxIo->data = hSession;
    ctxIo->thisClass = this;
    req = &ctxIo->req;
    req->data = ctxIo;
    iov = uv_buf_init(reinterpret_cast<char *>(ctxIo->buf), ctxIo->bufSize);
    ret = uv_fs_read(&daemon->loopMain, req, hUSB->bulkOut, &iov, 1, -1, OnUSBRead);
    if (ret < 0) {
        WRITE_LOG(LOG_FATAL, "uv_fs_read < 0");
        goto FAILED;
    }
    return 0;
FAILED:
    if (ctxIo != nullptr) {
        delete ctxIo;
    }
    if (buf != nullptr) {
        delete[] buf;
    }
    return -1;
}

// Because USB can connect to only one host，daemonUSB is only one Session by default
void HdcDaemonUSB::WatchEPTimer(uv_timer_t *handle)
{
    HdcDaemonUSB *thisClass = (HdcDaemonUSB *)handle->data;
    HUSB hUSB = thisClass->usbMain->hUSB;
    if (thisClass->isAlive) {
        return;  // ok not todo...
    }
    if (thisClass->ConnectEPPoint(hUSB)) {
        return;
    }
    // connect OK
    thisClass->isAlive = true;
    thisClass->LoopUSBRead(thisClass->usbMain);
}
}  // namespace Hdc