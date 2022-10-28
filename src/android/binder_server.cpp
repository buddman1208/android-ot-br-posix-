/*
 *    Copyright (c) 2023, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

#define OTBR_LOG_TAG "BINDER"

#include "android/binder_server.hpp"

#include <net/if.h>
#include <string.h>

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <openthread/border_router.h>
#include <openthread/ip6.h>
#include <openthread/openthread-system.h>
#include <openthread/platform/infra_if.h>

#include "common/code_utils.hpp"

#define BYTE_ARR_END(arr) ((arr) + sizeof(arr))

namespace otbr {
namespace Android {

static const char       OTBR_SERVICE_NAME[] = "ot_daemon";
static constexpr size_t kMaxIp6Size         = 1280;

static void PropagateResult(otError                                        aError,
                            const std::string                             &aMessage,
                            const std::shared_ptr<IEmptyResponseCallback> &aCallback)
{
    (aError == OT_ERROR_NONE) ? aCallback->onSuccess() : aCallback->onError(aError, aMessage);
}

static Ipv6AddressInfo ConvertToAddressInfo(const otIp6AddressInfo &aAddressInfo)
{
    Ipv6AddressInfo addrInfo;

    addrInfo.address.assign(aAddressInfo.mAddress->mFields.m8, BYTE_ARR_END(aAddressInfo.mAddress->mFields.m8));
    addrInfo.prefixLength = aAddressInfo.mPrefixLength;
    addrInfo.scope        = aAddressInfo.mScope;
    addrInfo.isPreferred  = aAddressInfo.mPreferred;
    return addrInfo;
}

BinderServer &BinderServer::GetInstance()
{
    static BinderServer service;

    return service;
}

void BinderServer::InitOrDie(otbr::Ncp::ControllerOpenThread *aNcp)
{
    binder_exception_t exp = AServiceManager_registerLazyService(asBinder().get(), OTBR_SERVICE_NAME);
    SuccessOrDie(exp, "Failed to register OT daemon binder service");

    mNcp = aNcp;
    mNcp->AddThreadStateChangedCallback([this](otChangedFlags aFlags) { StateCallback(aFlags); });
    otIp6SetAddressCallback(mNcp->GetInstance(), BinderServer::AddressCallback, this);
    otIp6SetReceiveCallback(mNcp->GetInstance(), BinderServer::ReceiveCallback, this);
}

void BinderServer::StateCallback(otChangedFlags aFlags)
{
    if (mCallback == nullptr)
    {
        otbrLogWarning("Ignoring OT state changes: callback is not set");
        ExitNow();
    }

    if (aFlags & OT_CHANGED_THREAD_NETIF_STATE)
    {
        mCallback->onInterfaceStateChanged(otIp6IsEnabled(GetOtInstance()));
    }

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        mCallback->onDeviceRoleChanged(otThreadGetDeviceRole(GetOtInstance()));

        if (!isAttached())
        {
            for (const auto &detachCallback : mOngoingDetachCallbacks)
            {
                detachCallback();
            }
            mOngoingDetachCallbacks.clear();
        }
    }

    if (aFlags & OT_CHANGED_THREAD_PARTITION_ID)
    {
        mCallback->onPartitionIdChanged(otThreadGetPartitionId(GetOtInstance()));
    }

    if (aFlags & OT_CHANGED_ACTIVE_DATASET)
    {
        std::vector<uint8_t>     result;
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetActiveTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            result.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        mCallback->onActiveOperationalDatasetChanged(result);
    }

    if (aFlags & OT_CHANGED_PENDING_DATASET)
    {
        std::vector<uint8_t>     result;
        otOperationalDatasetTlvs datasetTlvs;
        if (otDatasetGetPendingTlvs(GetOtInstance(), &datasetTlvs) == OT_ERROR_NONE)
        {
            result.assign(datasetTlvs.mTlvs, datasetTlvs.mTlvs + datasetTlvs.mLength);
        }
        mCallback->onPendingOperationalDatasetChanged(result);
    }

exit:
    return;
}

void BinderServer::AddressCallback(const otIp6AddressInfo *aAddressInfo, bool aIsAdded, void *aBinderServer)
{
    BinderServer *thisServer = static_cast<BinderServer *>(aBinderServer);

    if (thisServer->mCallback != nullptr)
    {
        thisServer->mCallback->onAddressChanged(ConvertToAddressInfo(*aAddressInfo), aIsAdded);
    }
    else
    {
        otbrLogWarning("OT daemon callback is not set");
    }
}

void BinderServer::ReceiveCallback(otMessage *aMessage, void *aBinderServer)
{
    static_cast<BinderServer *>(aBinderServer)->ReceiveCallback(aMessage);
}

void BinderServer::ReceiveCallback(otMessage *aMessage)
{
    char     packet[kMaxIp6Size];
    uint16_t length = otMessageGetLength(aMessage);
    int      fd     = mTunFd.get();

    if (otMessageRead(aMessage, 0, packet, sizeof(packet)) != length)
    {
        otbrLogWarning("Failed to read packet from otMessage");
        ExitNow();
    }

    if (write(fd, packet, length) != length)
    {
        otbrLogWarning("Failed to send packet over tunnel interface: %s", strerror(errno));
    }

exit:
    otMessageFree(aMessage);
}

void BinderServer::TransmitCallback(void)
{
    char              packet[kMaxIp6Size];
    ssize_t           length;
    otMessage        *message = nullptr;
    otError           error   = OT_ERROR_NONE;
    otMessageSettings settings;
    int               fd = mTunFd.get();

    VerifyOrExit(fd != -1);

    length = read(fd, packet, sizeof(packet));

    if (length == -1)
    {
        otbrLogWarning("Failed to read packet from tunnel interface: %s", strerror(errno));
        ExitNow();
    }
    else if (length == 0)
    {
        otbrLogWarning("Unexpected EOF on the tunnel FD");
        ExitNow();
    }

    VerifyOrExit(GetOtInstance() != nullptr, otbrLogWarning("Ignoring tunnel packet: OT is not initialized"));

    settings.mLinkSecurityEnabled = (otThreadGetDeviceRole(GetOtInstance()) != OT_DEVICE_ROLE_DISABLED);
    settings.mPriority            = OT_MESSAGE_PRIORITY_LOW;

    // FIXME(wgtdkp): this doesn't support NAT64, we should use a shared library with ot-posix
    // to handle packet translations between the tunnel interface and Thread.
    message = otIp6NewMessage(GetOtInstance(), &settings);
    VerifyOrExit(message != nullptr, error = OT_ERROR_NO_BUFS);

    SuccessOrExit(error = otMessageAppend(message, packet, length));

    error   = otIp6Send(GetOtInstance(), message);
    message = nullptr;

exit:
    if (message != nullptr)
    {
        otMessageFree(message);
    }

    if (error != OT_ERROR_NONE)
    {
        if (error == OT_ERROR_DROP)
        {
            otbrLogInfo("Dropped tunnel packet (length=%d)", length);
        }
        else
        {
            otbrLogWarning("Failed to transmit tunnel packet: %s", otThreadErrorToString(error));
        }
    }
}

otInstance *BinderServer::GetOtInstance()
{
    return mNcp->GetInstance();
}

void BinderServer::Update(MainloopContext &aMainloop)
{
    int fd = mTunFd.get();

    if (fd != -1)
    {
        FD_SET(fd, &aMainloop.mReadFdSet);
        aMainloop.mMaxFd = std::max(aMainloop.mMaxFd, fd);
    }
}

void BinderServer::Process(const MainloopContext &aMainloop)
{
    int fd = mTunFd.get();

    if (fd != -1 && FD_ISSET(fd, &aMainloop.mReadFdSet))
    {
        TransmitCallback();
    }
}

Status BinderServer::initialize(const ParcelFileDescriptor &aTunFd, const std::shared_ptr<IOtDaemonCallback> &aCallback)
{
    otbrLogDebug("OT daemon is initialized by the binder client (tunFd=%d)", aTunFd.get());

    mTunFd    = aTunFd.dup();
    mCallback = aCallback;
    return Status::ok();
}

Status BinderServer::attach(bool                                           aDoForm,
                            const std::vector<uint8_t>                    &aActiveOpDatasetTlvs,
                            const std::shared_ptr<IEmptyResponseCallback> &aCallback)
{
    otError                  error = OT_ERROR_NONE;
    std::string              message;
    otOperationalDatasetTlvs datasetTlvs;

    // TODO(b/273160198): check how we can implement attach-only behavior
    (void)aDoForm;

    otbrLogInfo("Start attaching...");

    VerifyOrExit(!isAttached(), error = OT_ERROR_INVALID_STATE, message = "Cannot attach when already attached");

    std::copy(aActiveOpDatasetTlvs.begin(), aActiveOpDatasetTlvs.end(), datasetTlvs.mTlvs);
    datasetTlvs.mLength = aActiveOpDatasetTlvs.size();
    SuccessOrExit(error   = otDatasetSetActiveTlvs(GetOtInstance(), &datasetTlvs),
                  message = "Failed to set Active Operational Dataset");

    // Shouldn't we have an equivalent `otThreadAttach` method vs `otThreadDetachGracefully`?
    SuccessOrExit(error = otIp6SetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread interface");
    SuccessOrExit(error = otThreadSetEnabled(GetOtInstance(), true), message = "Failed to bring up Thread stack");

exit:
    PropagateResult(error, message, aCallback);
    return Status::ok();
}

void BinderServer::detachGracefully(const DetachCallback &aCallback)
{
    otError error;

    mOngoingDetachCallbacks.push_back(aCallback);

    // The callback is already guarded by a timer inside OT
    error = otThreadDetachGracefully(GetOtInstance(), BinderServer::DetachGracefullyCallback, this);
    if (error == OT_ERROR_BUSY)
    {
        // There is already an ongoing detach request, do nothing but enqueue the callback
        otbrLogDebug("Reuse existing detach() request");
        ExitNow(error = OT_ERROR_NONE);
    }

exit:;
}

Status BinderServer::detach(const std::shared_ptr<IEmptyResponseCallback> &aCallback)
{
    detachGracefully([=]() { aCallback->onSuccess(); });
    return Status::ok();
}

void BinderServer::DetachGracefullyCallback(void *aBinderServer)
{
    BinderServer *thisServer = static_cast<BinderServer *>(aBinderServer);

    for (auto callback : thisServer->mOngoingDetachCallbacks)
    {
        callback();
    }
    thisServer->mOngoingDetachCallbacks.clear();
}

bool BinderServer::isAttached()
{
    otDeviceRole role = otThreadGetDeviceRole(GetOtInstance());

    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

Status BinderServer::scheduleMigration(const std::vector<uint8_t>                    &aPendingOpDatasetTlvs,
                                       const std::shared_ptr<IEmptyResponseCallback> &aCallback)
{
    otError              error = OT_ERROR_NONE;
    std::string          message;
    otOperationalDataset emptyDataset;

    VerifyOrExit(isAttached(), error = OT_ERROR_INVALID_STATE,
                 message = "Cannot schedule migration when this device is detached");

    error = otDatasetSendMgmtPendingSet(GetOtInstance(), &emptyDataset, aPendingOpDatasetTlvs.data(),
                                        aPendingOpDatasetTlvs.size(), sendMgmtPendingSetCallback,
                                        /* aBinderServer= */ nullptr);
    if (error != OT_ERROR_NONE)
    {
        message = "Failed to send MGMT_PENDING_SET.req";
    }

exit:
    PropagateResult(error, message, aCallback);
    return Status::ok();
}

void BinderServer::sendMgmtPendingSetCallback(otError aResult, void *aBinderServer)
{
    (void)aBinderServer;

    otbrLogDebug("otDatasetSendMgmtPendingSet callback: %d", aResult);
}

Status BinderServer::getExtendedMacAddress(std::vector<uint8_t> *aExtendedMacAddress)
{
    const otExtAddress *extAddress = otLinkGetExtendedAddress(GetOtInstance());

    aExtendedMacAddress->assign(extAddress->m8, extAddress->m8 + sizeof(extAddress->m8));
    return Status::ok();
}

Status BinderServer::getStandardVersion(int *aStandardVersion)
{
    *aStandardVersion = otThreadGetVersion();
    return Status::ok();
}

} // namespace Android
} // namespace otbr
