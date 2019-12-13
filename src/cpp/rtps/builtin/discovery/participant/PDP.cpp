// Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file PDP.cpp
 *
 */

#include <fastrtps/rtps/builtin/discovery/participant/PDP.h>
#include <fastrtps/rtps/builtin/discovery/participant/PDPListener.h>

#include <fastrtps/rtps/builtin/BuiltinProtocols.h>
#include <fastrtps/rtps/builtin/liveliness/WLP.h>

#include <fastrtps/rtps/builtin/data/ParticipantProxyData.h>
#include <fastrtps/rtps/participant/RTPSParticipantListener.h>
#include <fastrtps/rtps/resources/TimedEvent.h>
#include <fastrtps/rtps/builtin/data/ReaderProxyData.h>
#include <fastrtps/rtps/builtin/data/WriterProxyData.h>

#include <fastrtps/rtps/builtin/discovery/endpoint/EDPSimple.h>
#include <fastrtps/rtps/builtin/discovery/endpoint/EDPStatic.h>

#include <fastrtps/rtps/resources/AsyncWriterThread.h>

#include "../../../participant/RTPSParticipantImpl.h"

#include <fastrtps/rtps/writer/StatelessWriter.h>
#include <fastrtps/rtps/reader/StatelessReader.h>
#include <fastrtps/rtps/reader/StatefulReader.h>

#include <fastrtps/rtps/history/WriterHistory.h>
#include <fastrtps/rtps/history/ReaderHistory.h>


#include <fastrtps/utils/TimeConversion.h>
#include <fastrtps/utils/IPLocator.h>

#include <fastrtps/log/Log.h>

#include <mutex>
#include <chrono>

namespace eprosima {
namespace fastrtps {
namespace rtps {

// Default configuration values for PDP reliable entities.

const Duration_t pdp_heartbeat_period{ 0, 350 * 1000  }; // 350 milliseconds
const Duration_t pdp_nack_response_delay{ 0, 100 * 1000  }; // 100 milliseconds
const Duration_t pdp_nack_supression_duration{ 0, 11 * 1000 }; // ~11 milliseconds
const Duration_t pdp_heartbeat_response_delay{ 0, 11 * 1000 }; // ~11 milliseconds

const int32_t pdp_initial_reserved_caches = 20;

// Static pool resources shared among all participants
std::recursive_mutex PDP::pool_mutex_;
size_t PDP::pdp_counter_ = 0;

size_t PDP::participant_proxies_data_number_ = 0;
std::vector<ParticipantProxyData*> PDP::participant_proxies_data_pool_;

size_t PDP::reader_proxies_number_ = 0;
std::vector<ReaderProxyData*> PDP::reader_proxies_pool_;

size_t PDP::writer_proxies_number_ = 0;
std::vector<WriterProxyData*> PDP::writer_proxies_pool_;

std::map<GuidPrefix_t, std::weak_ptr<ParticipantProxyData>> PDP::pool_participant_references_;
std::map<GUID_t, std::weak_ptr<ReaderProxyData>> PDP::pool_reader_references_;
std::map<GUID_t, std::weak_ptr<WriterProxyData>> PDP::pool_writer_references_;

PDP::PDP (
        BuiltinProtocols* built,
        const RTPSParticipantAllocationAttributes& allocation)
    : mp_builtin(built)
    , mp_RTPSParticipant(nullptr)
    , mp_PDPWriter(nullptr)
    , mp_PDPReader(nullptr)
    , mp_EDP(nullptr)
    , m_hasChangedLocalPDP(true)
    , mp_listener(nullptr)
    , mp_PDPWriterHistory(nullptr)
    , mp_PDPReaderHistory(nullptr)
    , temp_reader_data_(allocation.locators.max_unicast_locators, allocation.locators.max_multicast_locators)
    , temp_writer_data_(allocation.locators.max_unicast_locators, allocation.locators.max_multicast_locators)
    , mp_mutex(new std::recursive_mutex())
    , participant_proxies_(allocation.participants)
    , participant_proxies_number_(allocation.participants.initial)
    , resend_participant_info_event_(nullptr)
{
    // reserve room in global variables
    initialize_or_update_pool_allocation(allocation);

    // reserve room in local ones
    participant_proxies_pool_.reserve(participant_proxies_number_);
    for(size_t i = 0; i < participant_proxies_number_; ++i)
    {
        participant_proxies_pool_.push_back(new ParticipantProxy(allocation));
    }

}

PDP::~PDP()
{
    delete resend_participant_info_event_;
    mp_RTPSParticipant->disableReader(mp_PDPReader);
    delete mp_EDP;
    mp_RTPSParticipant->deleteUserEndpoint(mp_PDPWriter);
    mp_RTPSParticipant->deleteUserEndpoint(mp_PDPReader);
    delete mp_PDPWriterHistory;
    delete mp_PDPReaderHistory;
    delete mp_listener;

    // Free those proxies on actual use
    for(ParticipantProxy* p : participant_proxies_)
    {
        delete p;
    }

    // Free those proxies unused
    for(ParticipantProxy* p : participant_proxies_pool_)
    {
        delete p;
    }

    delete mp_mutex;

    remove_pool_resources();
}

ParticipantProxy* PDP::add_participant_proxy(
    std::shared_ptr<ParticipantProxyData>& ppd,
    bool with_lease_duration /* = true*/)
{
    ParticipantProxy* ret_val = nullptr;

    if(!ppd)
    {
        return nullptr;
    }

    // Lock on the participant proxy data
    std::unique_lock<std::recursive_mutex> ppd_lock(ppd->ppd_mutex_);

    // Get the ParticipantProxy associated, note PDP mutex is supposed to be locked
    if(participant_proxies_pool_.empty())
    {
        size_t max_proxies = participant_proxies_.max_size();
        if(participant_proxies_number_ < max_proxies)
        {
            // Pool is empty but limit has not been reached, so we create a new entry.
            ret_val = new ParticipantProxy(mp_RTPSParticipant->getRTPSParticipantAttributes().allocation);

            if(nullptr == ret_val)
            {
                return nullptr;
            }

            // Create the event always because its shared with other participants
            if(ppd->m_guid != mp_RTPSParticipant->getGuid())
            {
                ret_val->set_lease_duration_event(new TimedEvent(mp_RTPSParticipant->getEventResource(),
                    [this, ret_val]() -> bool
                {
                    if(ret_val->get_ppd())
                    {
                        check_remote_participant_liveliness(ret_val);
                    }
                    return false;
                }, 0.0));
            }

            ++participant_proxies_number_;
        }
    }
    else
    {
        // Pool is not empty, use entry from pool
        ret_val = participant_proxies_pool_.back();
        participant_proxies_pool_.pop_back();
    }

    // Update lease duration event for other participants
    auto event = ret_val->get_lease_duration_event();
    if(event)
    {
        event->update_interval(ppd->lease_duration_);
        event->restart_timer();
    }
    // associate the local object with the global one
    ret_val->set_ppd(std::move(ppd));
    // update lease duration properties
    ret_val->should_check_lease_duration_ = with_lease_duration;
    // add to the local collection
    participant_proxies_.push_back(ret_val);
    
    ppd_lock.release(); // on sucess returns with the ppd lock owned
    return ret_val;
}

ParticipantProxy* PDP::add_participant_proxy(
    const GUID_t& participant_guid,
    bool with_lease_duration)
{
    std::shared_ptr<ParticipantProxyData> ppd;

    // Get the ParticipantProxyData associated
    {
        std::unique_lock<std::recursive_mutex> pool_guard(PDP::pool_mutex_);

        //See whether it is already in use
        auto participant_reference = pool_participant_references_.find(participant_guid.guidPrefix);
        if(participant_reference != pool_participant_references_.end())
        {
            // take a strong reference
            ppd = participant_reference->second.lock();

            // It should be an associated ParticipantProxyData
            assert(!!ppd);
        }
        else
        {
            // Try to take one entry from the pool
            if(participant_proxies_data_pool_.empty())
            {
                size_t max_proxies = participant_proxies_.max_size();
                if(participant_proxies_data_number_ < max_proxies)
                {
                    // Pool is empty but limit has not been reached, so we create a new entry.
                    ppd = std::shared_ptr<ParticipantProxyData>(
                        new ParticipantProxyData(mp_RTPSParticipant->getRTPSParticipantAttributes().allocation),
                        ParticipantProxyData::pool_deleter());

                    if(!ppd)
                    {
                        return nullptr;
                    }

                    ++participant_proxies_data_number_;
                }
                else
                {
                    logWarning(RTPS_PDP, "Maximum number of participant proxies (" << max_proxies << \
                        ") reached for participant " << mp_RTPSParticipant->getGuid() << std::endl);
                    return nullptr;
                }
            }
            else
            {
                // Pool is not empty, use entry from pool
                ppd.reset(participant_proxies_data_pool_.back(), ParticipantProxyData::pool_deleter());
                participant_proxies_data_pool_.pop_back();
            }

            // locks on the ParticipantProxyData, this method exits with this lock taken
            std::lock_guard<std::recursive_mutex> lock(ppd->ppd_mutex_);
            ppd->m_guid = participant_guid;
        }

        // Add returned entry to the collection
        pool_participant_references_[participant_guid.guidPrefix] = std::weak_ptr<ParticipantProxyData>(ppd);
    }

    // add to the local ParticipantProxy collection. This method returns with the ppd lock taken on success
    return add_participant_proxy(ppd, with_lease_duration);
}

void PDP::initializeParticipantProxyData(ParticipantProxyData* participant_data)
{
    std::lock_guard<std::recursive_mutex> ppd_lock(participant_data->ppd_mutex_);

    // Signal out is the first announcement to avoid deserialization from all other intra process participants
    participant_data->version_ = SequenceNumber_t(0, 1);

    participant_data->lease_duration_ = mp_RTPSParticipant->getAttributes().builtin.discovery_config.leaseDuration;
    //set_VendorId_eProsima(participant_data->m_VendorId);
    participant_data->m_VendorId = c_VendorId_eProsima;

    participant_data->m_availableBuiltinEndpoints |= DISC_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER;
    participant_data->m_availableBuiltinEndpoints |= DISC_BUILTIN_ENDPOINT_PARTICIPANT_DETECTOR;

#if HAVE_SECURITY
    participant_data->m_availableBuiltinEndpoints |= DISC_BUILTIN_ENDPOINT_PARTICIPANT_SECURE_ANNOUNCER;
    participant_data->m_availableBuiltinEndpoints |= DISC_BUILTIN_ENDPOINT_PARTICIPANT_SECURE_DETECTOR;
#endif

    if(mp_RTPSParticipant->getAttributes().builtin.use_WriterLivelinessProtocol)
    {
        participant_data->m_availableBuiltinEndpoints |= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER;
        participant_data->m_availableBuiltinEndpoints |= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER;

#if HAVE_SECURITY
        participant_data->m_availableBuiltinEndpoints |= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_WRITER;
        participant_data->m_availableBuiltinEndpoints |= BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_SECURE_DATA_READER;
#endif
    }

#if HAVE_SECURITY
    participant_data->m_availableBuiltinEndpoints |= mp_RTPSParticipant->security_manager().builtin_endpoints();
#endif

    for (const Locator_t& loc : mp_RTPSParticipant->getAttributes().defaultUnicastLocatorList)
    {
        participant_data->default_locators.add_unicast_locator(loc);
    }
    for (const Locator_t& loc : mp_RTPSParticipant->getAttributes().defaultMulticastLocatorList)
    {
        participant_data->default_locators.add_multicast_locator(loc);
    }
    participant_data->m_expectsInlineQos = false;
    participant_data->m_guid = mp_RTPSParticipant->getGuid();
    for(uint8_t i = 0; i<16; ++i)
    {
        if(i<12)
            participant_data->m_key.value[i] = participant_data->m_guid.guidPrefix.value[i];
        else
            participant_data->m_key.value[i] = participant_data->m_guid.entityId.value[i - 12];
    }

    // Keep persistence Guid_Prefix_t in a specific property. This info must be propagated to all builtin endpoints
    {
        GuidPrefix_t persistent = mp_RTPSParticipant->getAttributes().prefix;

        if(persistent != c_GuidPrefix_Unknown)
        {
            participant_data->set_persistence_guid(
                GUID_t(
                    persistent,
                    c_EntityId_RTPSParticipant));
        }
    }

    participant_data->metatraffic_locators.unicast.clear();
    for (const Locator_t& loc : this->mp_builtin->m_metatrafficUnicastLocatorList)
    {
        participant_data->metatraffic_locators.add_unicast_locator(loc);
    }

    participant_data->metatraffic_locators.multicast.clear();
    if (!m_discovery.avoid_builtin_multicast || participant_data->metatraffic_locators.unicast.empty())
    {
        for(const Locator_t& loc: this->mp_builtin->m_metatrafficMulticastLocatorList)
        {
            participant_data->metatraffic_locators.add_multicast_locator(loc);
        }
    }

    participant_data->m_participantName = std::string(mp_RTPSParticipant->getAttributes().getName());

    participant_data->m_userData = mp_RTPSParticipant->getAttributes().userData;

#if HAVE_SECURITY
    IdentityToken* identity_token = nullptr;
    if(mp_RTPSParticipant->security_manager().get_identity_token(&identity_token) && identity_token != nullptr)
    {
        participant_data->identity_token_ = std::move(*identity_token);
        mp_RTPSParticipant->security_manager().return_identity_token(identity_token);
    }

    PermissionsToken* permissions_token = nullptr;
    if(mp_RTPSParticipant->security_manager().get_permissions_token(&permissions_token)
        && permissions_token != nullptr)
    {
        participant_data->permissions_token_ = std::move(*permissions_token);
        mp_RTPSParticipant->security_manager().return_permissions_token(permissions_token);
    }

    if (mp_RTPSParticipant->is_secure())
    {
        const security::ParticipantSecurityAttributes & sec_attrs = mp_RTPSParticipant->security_attributes();
        participant_data->security_attributes_ = sec_attrs.mask();
        participant_data->plugin_security_attributes_ = sec_attrs.plugin_participant_attributes;
    }
    else
    {
        participant_data->security_attributes_ = 0UL;
        participant_data->plugin_security_attributes_ = 0UL;
    }
#endif
}

bool PDP::initPDP(
    RTPSParticipantImpl* part)
{
    logInfo(RTPS_PDP,"Beginning");
    mp_RTPSParticipant = part;
    m_discovery = mp_RTPSParticipant->getAttributes().builtin;
    initial_announcements_ = m_discovery.discovery_config.initial_announcements;
    //CREATE ENDPOINTS
    if (!createPDPEndpoints())
    {
        return false;
    }
    //UPDATE METATRAFFIC.
    mp_builtin->updateMetatrafficLocators(this->mp_PDPReader->getAttributes().unicastLocatorList);
    ParticipantProxy * pdata = add_participant_proxy(part->getGuid(), true);

    if (!pdata)
    {
        return false;
    }

    pdata->get_ppd_mutex().unlock(); // add_participant_proxy_data locks on ParticipantProxyData mutex
    // nobody knows about him thus we can unlock

    initializeParticipantProxyData(pdata->get_ppd().get());

    resend_participant_info_event_ = new TimedEvent(mp_RTPSParticipant->getEventResource(),
            [&]() -> bool
            {
                announceParticipantState(false);
                set_next_announcement_interval();
                return true;
            },
            0);

    set_initial_announcement_interval();

    return true;
}

bool PDP::enable()
{
    return mp_RTPSParticipant->enableReader(mp_PDPReader);
}

void PDP::announceParticipantState(
    bool new_change,
    bool dispose,
    WriteParams& wparams)
{
    logInfo(RTPS_PDP,"Announcing RTPSParticipant State (new change: "<< new_change <<")");
    CacheChange_t* change = nullptr;

    if(!dispose)
    {
        if(m_hasChangedLocalPDP.exchange(false) || new_change)
        {
            mp_mutex->lock();
            ParticipantProxy* local_participant_data = getLocalParticipantProxy();
            InstanceHandle_t key = local_participant_data->get_ppd()->m_key;
            ParticipantProxyData proxy_data_copy(*local_participant_data->get_ppd());
            mp_mutex->unlock();

            if(mp_PDPWriterHistory->getHistorySize() > 0)
                mp_PDPWriterHistory->remove_min_change();
            // TODO(Ricardo) Change DISCOVERY_PARTICIPANT_DATA_MAX_SIZE with getLocalParticipantProxyData()->size().
            change = mp_PDPWriter->new_change([]() -> uint32_t
                {
                    return DISCOVERY_PARTICIPANT_DATA_MAX_SIZE;
                }
            , ALIVE, key);

            if(change != nullptr)
            {
                CDRMessage_t aux_msg(change->serializedPayload);

#if __BIG_ENDIAN__
                change->serializedPayload.encapsulation = (uint16_t)PL_CDR_BE;
                aux_msg.msg_endian = BIGEND;
#else
                change->serializedPayload.encapsulation = (uint16_t)PL_CDR_LE;
                aux_msg.msg_endian =  LITTLEEND;
#endif

                if (proxy_data_copy.writeToCDRMessage(&aux_msg, true))
                {
                    change->serializedPayload.length = (uint16_t)aux_msg.length;

                   mp_PDPWriterHistory->add_change(change, wparams);
                }
                else
                {
                    logError(RTPS_PDP, "Cannot serialize ParticipantProxyData.");
                }
            }
        }

    }
    else
    {
        this->mp_mutex->lock();
        ParticipantProxyData proxy_data_copy(*getLocalParticipantProxy()->get_ppd());
        this->mp_mutex->unlock();

        if(mp_PDPWriterHistory->getHistorySize() > 0)
            mp_PDPWriterHistory->remove_min_change();
        change = mp_PDPWriter->new_change([]() -> uint32_t
            {
                return DISCOVERY_PARTICIPANT_DATA_MAX_SIZE;
            }
        , NOT_ALIVE_DISPOSED_UNREGISTERED, getLocalParticipantProxy()->get_ppd()->m_key);

        if(change != nullptr)
        {
            CDRMessage_t aux_msg(change->serializedPayload);

#if __BIG_ENDIAN__
            change->serializedPayload.encapsulation = (uint16_t)PL_CDR_BE;
            aux_msg.msg_endian = BIGEND;
#else
            change->serializedPayload.encapsulation = (uint16_t)PL_CDR_LE;
            aux_msg.msg_endian =  LITTLEEND;
#endif

            if (proxy_data_copy.writeToCDRMessage(&aux_msg, true))
            {
                change->serializedPayload.length = (uint16_t)aux_msg.length;

                mp_PDPWriterHistory->add_change(change, wparams);
            }
            else
            {
                logError(RTPS_PDP, "Cannot serialize ParticipantProxyData.");
            }
        }
    }
}

void PDP::stopParticipantAnnouncement()
{
    resend_participant_info_event_->cancel_timer();
}

void PDP::resetParticipantAnnouncement()
{
    resend_participant_info_event_->restart_timer();
}

bool PDP::has_reader_proxy(const GUID_t& reader)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == reader.guidPrefix)
        {
            for (std::shared_ptr<ReaderProxyData>& rit : pit->readers_)
            {
                if (rit->guid() == reader)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool PDP::lookupReaderProxyData(const GUID_t& reader, ReaderProxyData& rdata)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == reader.guidPrefix)
        {
            for (std::shared_ptr<ReaderProxyData>& rit : pit->readers_)
            {
                if (rit->guid() == reader)
                {
                    auto lck = rit->unique_lock();
                    rdata.copy(rit.get());
                    return true;
                }
            }
        }
    }
    return false;
}

bool PDP::has_writer_proxy_data(const GUID_t& writer)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*this->mp_mutex);
    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == writer.guidPrefix)
        {
            for (std::shared_ptr<WriterProxyData>& wit : pit->writers_)
            {
                if (wit->guid() == writer)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool PDP::lookupWriterProxyData(const GUID_t& writer, WriterProxyData& wdata)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == writer.guidPrefix)
        {
            for (std::shared_ptr<WriterProxyData>& wit : pit->writers_)
            {
                if (wit->guid() == writer)
                {
                    auto lk = wit->unique_lock();
                    wdata.copy(wit.get());
                    return true;
                }
            }
        }
    }
    return false;
}

std::shared_ptr<WriterProxyData> PDP::lookupWriterProxyData(const GUID_t& writer)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == writer.guidPrefix)
        {
            for(std::shared_ptr<WriterProxyData>& wit : pit->writers_)
            {
                if(wit->guid() == writer)
                {
                    return wit;
                }
            }
        }
    }
    return std::shared_ptr<WriterProxyData>();
}

std::shared_ptr<ReaderProxyData> PDP::lookupReaderProxyData(const GUID_t& reader)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == reader.guidPrefix)
        {
            for(std::shared_ptr<ReaderProxyData>& rit : pit->readers_)
            {
                if(rit->guid() == reader)
                {
                    return rit;
                }
            }
        }
    }
    return std::shared_ptr<ReaderProxyData>();
}

bool PDP::removeReaderProxyData(const GUID_t& reader_guid)
{
    logInfo(RTPS_PDP, "Removing reader proxy data " << reader_guid);
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == reader_guid.guidPrefix)
        {
            for (std::shared_ptr<ReaderProxyData> rit : pit->readers_)
            {
                if (rit->guid() == reader_guid)
                {
                    mp_EDP->unpairReaderProxy(pit->get_guid(), reader_guid);

                    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
                    if (listener)
                    {
                        auto lk = rit->unique_lock();

                        ReaderDiscoveryInfo info(*rit);
                        info.status = ReaderDiscoveryInfo::REMOVED_READER;
                        listener->onReaderDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                    }

                    // free the strong reference
                    pit->readers_.remove(rit);

                    return true;
                }
            }
        }
    }

    return false;
}

bool PDP::removeWriterProxyData(const GUID_t& writer_guid)
{
    logInfo(RTPS_PDP, "Removing writer proxy data " << writer_guid);
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid_prefix() == writer_guid.guidPrefix)
        {
            for (std::shared_ptr<WriterProxyData>& wit : pit->writers_)
            {
                if (wit->guid() == writer_guid)
                {
                    mp_EDP->unpairWriterProxy(pit->get_guid(), writer_guid);

                    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
                    if (listener)
                    {
                        wit->unique_lock();

                        WriterDiscoveryInfo info(*wit);
                        info.status = WriterDiscoveryInfo::REMOVED_WRITER;
                        listener->onWriterDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                    }

                    // remove strong reference to the proxy data
                    pit->writers_.remove(wit);
          
                    return true;
                }
            }
        }
    }

    return false;
}

bool PDP::lookup_participant_name(
        const GUID_t& guid,
        string_255& name)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    for (ParticipantProxy* pit : participant_proxies_)
    {
        if (pit->get_guid() == guid)
        {
            std::lock_guard<std::recursive_mutex> lock(pit->proxy_data_->ppd_mutex_);
            name = pit->proxy_data_->m_participantName;
            return true;
        }
    }
    return false;
}

bool PDP::lookup_participant_key(
        const GUID_t& participant_guid,
        InstanceHandle_t& key)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid() == participant_guid)
        {
            std::lock_guard<std::recursive_mutex> lock(pit->proxy_data_->ppd_mutex_);
            key = pit->proxy_data_->m_key;
            return true;
        }
    }
    return false;
}

std::shared_ptr<ReaderProxyData> PDP::addReaderProxyData(
        const GUID_t& reader_guid,
        GUID_t& participant_guid,
        std::function<bool(ReaderProxyData*, bool, const ParticipantProxyData&)> initializer_func)
{
    logInfo(RTPS_PDP, "Adding reader proxy data " << reader_guid);

    std::shared_ptr<ReaderProxyData> ret_val;
    participant_guid = GUID_t::unknown();
    ParticipantProxy* pp = nullptr;

    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    // This method is called also for updates, thus, we first look for it in the known ones
    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == reader_guid.guidPrefix)
        {
            // Copy participant data to be used outside.
            participant_guid = pit->get_guid();
            pp = pit;

            // Check that it is not already there:
            for(std::shared_ptr<ReaderProxyData>& rit : pit->readers_)
            {
                if(rit->guid().entityId == reader_guid.entityId)
                {
                    std::lock_guard<std::recursive_mutex> ppd_lock(pit->proxy_data_->ppd_mutex_);
                    auto rul = rit->unique_lock();

                    // update the proxy data
                    if(!initializer_func(rit.get(), true, *pit->proxy_data_))
                    {
                        return nullptr;
                    }

                    ret_val = rit;

                    // notify the update
                    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
                    if(listener)
                    {
                        ReaderDiscoveryInfo info(*ret_val);
                        info.status = ReaderDiscoveryInfo::CHANGED_QOS_READER;
                        listener->onReaderDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                    }

                    rul.release(); // If succeeds returns with the reader proxy lock
                    return ret_val;
                }
            }
        }
    }

    // The participant must be there
    //assert(pp != nullptr);
    //assert(participant_guid != GUID_t::unknown());

    // Get Participant allocation policies just in case we have to create a new one
    const RTPSParticipantAttributes& part_att = getRTPSParticipant()->getRTPSParticipantAttributes();

    // search into the readers pool
    if(!(ret_val = PDP::get_from_reader_proxy_pool(
        reader_guid,
        part_att.allocation.locators.max_unicast_locators,
        part_att.allocation.locators.max_multicast_locators)))
    {
        return nullptr;
    }

    // Add to ParticipantProxy
    pp->readers_.push_back(ret_val);

    std::unique_lock<std::recursive_mutex> ppd_lock(pp->proxy_data_->ppd_mutex_);
    auto rul = ret_val->unique_lock();

    if (!initializer_func(ret_val.get(), false, *pp->proxy_data_))
    {
        return nullptr;
    }

    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
    if(listener)
    {
        ReaderDiscoveryInfo info(*ret_val);
        info.status = ReaderDiscoveryInfo::DISCOVERED_READER;
        listener->onReaderDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
    }

    rul.release(); // If succeeds returns with the reader proxy data locked
    return ret_val;

 }

std::shared_ptr<WriterProxyData> PDP::add_builtin_writer_proxy_data(
    const WriterProxyData & wdata)
{
    // Check for a local copy
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    std::shared_ptr<WriterProxyData> wpd;
    ParticipantProxy * pp = nullptr;

    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == wdata.guid().guidPrefix)
        {
            // Copy participant data to be used outside.
            pp = pit;

            // Check that it is not already there:
            for(std::shared_ptr<WriterProxyData>& wit : pit->builtin_writers_)
            {
                if(wit->guid().entityId == wdata.guid().entityId)
                {
                    wpd = wit;
                }
            }
        }
    }

    if(nullptr == pp)
    {
        // Unknown participant, nonsensical behaviour
        assert(pp != nullptr);
        return nullptr;
    }

    if(wpd)
    {
        // already there
        return wpd;
    }

    // We need to add a local reference, assess if a global object is available
    wpd = PDP::get_alived_writer_proxy(wdata.guid());
    bool needs_copy = false;

    if(!wpd)
    {
        // search into the readers pool
        if(!(wpd = PDP::get_from_writer_proxy_pool(
            wdata.guid(),
            wdata.remote_locators().unicast.capacity(),
            wdata.remote_locators().multicast.capacity())))
        {
            return nullptr;
        }

        // we need to copy it
        needs_copy = true;
    }

    // Copy if needed, thats the first object and sync is not needed
    if(needs_copy)
    {
        wpd->copy(&wdata);
    }

    // add to the local participant collection in order to keep the global
    // object alive. Note that WriterProxies keep only weak references
    pp->builtin_writers_.push_back(wpd);

    return wpd;
}

std::shared_ptr<ReaderProxyData> PDP::add_builtin_reader_proxy_data(
    const ReaderProxyData & wdata)
{
    // Check for a local copy
    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);
    std::shared_ptr<ReaderProxyData> rpd;
    ParticipantProxy * pp = nullptr;

    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == wdata.guid().guidPrefix)
        {
            // Copy participant data to be used outside.
            pp = pit;

            // Check that it is not already there:
            for(std::shared_ptr<ReaderProxyData>& rit : pit->builtin_readers_)
            {
                if(rit->guid().entityId == wdata.guid().entityId)
                {
                    rpd = rit;
                }
            }
        }
    }

    if(nullptr == pp)
    {
        // Unknown participant, nonsensical behaviour
        assert(pp != nullptr);
        return nullptr;
    }

    if(rpd)
    {
        // already there
        return rpd;
    }

    // We need to add a local reference, assess if a global object is available
    rpd = PDP::get_alived_reader_proxy(wdata.guid());
    bool needs_copy = false;

    if(!rpd)
    {
        // search into the readers pool
        if(!(rpd = PDP::get_from_reader_proxy_pool(
            wdata.guid(),
            wdata.remote_locators().unicast.capacity(),
            wdata.remote_locators().multicast.capacity())))
        {
            return nullptr;
        }

        // we need to copy it
        needs_copy = true;
    }

    // Copy if needed, thats the first object and sync is not needed
    if(needs_copy)
    {
        rpd->copy(&wdata);
    }

    // add to the local participant collection in order to keep the global
    // object alive. Note that ReaderProxies keep only weak references
    pp->builtin_readers_.push_back(rpd);

    return rpd;
}

std::shared_ptr<WriterProxyData> PDP::addWriterProxyData(
    const GUID_t& writer_guid,
    GUID_t& participant_guid,
    std::function<bool(WriterProxyData*, bool, const ParticipantProxyData&)> initializer_func)
{
    logInfo(RTPS_PDP, "Adding writer proxy data " << writer_guid);

    std::shared_ptr<WriterProxyData> ret_val;
    participant_guid = GUID_t::unknown();
    ParticipantProxy* pp = nullptr;

    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    // This method is called also for updates, thus, we first look for it in the known ones
    for(ParticipantProxy* pit : participant_proxies_)
    {
        if(pit->get_guid_prefix() == writer_guid.guidPrefix)
        {
            // Copy participant data to be used outside.
            participant_guid = pit->get_guid();
            pp = pit;

            // Check that it is not already there:
            for(std::shared_ptr<WriterProxyData>& wit : pit->writers_)
            {
                if(wit->guid().entityId == writer_guid.entityId)
                {
                    std::lock_guard<std::recursive_mutex> ppd_lock(pit->proxy_data_->ppd_mutex_);
                    auto wul = wit->unique_lock();

                    // update the proxy data
                    if(!initializer_func(wit.get(), true, *pit->proxy_data_))
                    {
                        return nullptr;
                    }

                    ret_val = wit;

                    // notify the update
                    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
                    if(listener)
                    {
                        WriterDiscoveryInfo info(*ret_val);
                        info.status = WriterDiscoveryInfo::CHANGED_QOS_WRITER;
                        listener->onWriterDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                    }

                    wul.release(); // retval is valid thus we return with writer proxy locked
                    return ret_val;
                }
            }
        }
    }

    // The participant must be there
    assert(pp != nullptr && participant_guid != GUID_t::unknown());

    // Get Participant allocation policies just in case we have to create a new one
    const RTPSParticipantAttributes& part_att = getRTPSParticipant()->getRTPSParticipantAttributes();

    // search into the readers pool
    if(!(ret_val = PDP::get_from_writer_proxy_pool(
        writer_guid,
        part_att.allocation.locators.max_unicast_locators,
        part_att.allocation.locators.max_multicast_locators)))
    {
        return nullptr;
    }
 
    // Add to ParticipantProxyData
    pp->writers_.push_back(ret_val);

    std::unique_lock<std::recursive_mutex> ppd_lock(pp->proxy_data_->ppd_mutex_);
    auto wul = ret_val->unique_lock();

    if (!initializer_func(ret_val.get(), false, *pp->proxy_data_))
    {
        return nullptr;
    }

    RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();
    if (listener)
    {
        WriterDiscoveryInfo info(*ret_val);
        info.status = WriterDiscoveryInfo::DISCOVERED_WRITER;
        listener->onWriterDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
    }

    wul.release(); // If succeeds returns with the writer proxy data locked
    return ret_val;
    
}

bool PDP::remove_remote_participant(
        const GUID_t& partGUID,
        ParticipantDiscoveryInfo::DISCOVERY_STATUS reason)
{
    GUID_t local = getRTPSParticipant()->getGuid();

    if ( partGUID == local )
    {   // avoid removing our own data
        return false;
    }

    logInfo(RTPS_PDP,partGUID );
    ParticipantProxy* pdata = nullptr;

    //Remove it from our vector or RTPSParticipantProxies:
    std::unique_lock<std::recursive_mutex> pdp_lock(*mp_mutex);

    for(ParticipantProxy* pp : participant_proxies_)
    {
        if(pp->get_guid() == partGUID)
        {
            pdata = pp;
            break;
        }
    }

    if(pdata)
    {
        participant_proxies_.remove(pdata);
    }
    else
    {
        // nothing to remove
        return false;
    }

    pdp_lock.unlock(); // this proxy is not on the list any longer,
    // thus, no other thread on this pdp can reach it

    if(mp_EDP!=nullptr)
    {
        RTPSParticipantListener* listener = mp_RTPSParticipant->getListener();

        for(std::shared_ptr<ReaderProxyData>& rit : pdata->readers_)
        {
            GUID_t reader_guid(rit->guid());
            if (reader_guid != c_Guid_Unknown)
            {
                mp_EDP->unpairReaderProxy(partGUID, reader_guid);

                if (listener)
                {
                    auto lock = rit->unique_lock();

                    ReaderDiscoveryInfo info(*rit);
                    info.status = ReaderDiscoveryInfo::REMOVED_READER;
                    listener->onReaderDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                }
            }
        }

        for(std::shared_ptr<WriterProxyData>& wit : pdata->writers_)
        {
            GUID_t writer_guid(wit->guid());
            if (writer_guid != c_Guid_Unknown)
            {
                mp_EDP->unpairWriterProxy(partGUID, writer_guid);

                if (listener)
                {
                    auto lock = wit->unique_lock();

                    WriterDiscoveryInfo info(*wit);
                    info.status = WriterDiscoveryInfo::REMOVED_WRITER;
                    listener->onWriterDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
                }
            }
        }
    }

    {
        std::lock_guard<std::recursive_mutex> ppd_lock(pdata->get_ppd_mutex());

        if(mp_builtin->mp_WLP != nullptr)
            mp_builtin->mp_WLP->removeRemoteEndpoints(pdata->get_ppd().get());
        mp_EDP->removeRemoteEndpoints(pdata->get_ppd().get());
        removeRemoteEndpoints(pdata->get_ppd().get());

    }

#if HAVE_SECURITY
    mp_builtin->mp_participantImpl->security_manager().remove_participant(*pdata);
#endif

    mp_PDPReaderHistory->getMutex()->lock();
    for(std::vector<CacheChange_t*>::iterator it = mp_PDPReaderHistory->changesBegin();
            it!= mp_PDPReaderHistory->changesEnd(); ++it)
    {
        if((*it)->instanceHandle == pdata->proxy_data_->m_key)
        {
            mp_PDPReaderHistory->remove_change(*it);
            break;
        }
    }
    mp_PDPReaderHistory->getMutex()->unlock();

    auto listener =  mp_RTPSParticipant->getListener();
    if (listener != nullptr)
    {
        std::lock_guard<std::mutex> lock(callback_mtx_);
        std::lock_guard<std::recursive_mutex> ppd_lock(pdata->proxy_data_->ppd_mutex_);

        ParticipantDiscoveryInfo info(*pdata->proxy_data_);
        info.status = reason;
        listener->onParticipantDiscovery(mp_RTPSParticipant->getUserRTPSParticipant(), std::move(info));
    }

    // By clearing the participant proxy we remove the strong references
    // to the global reader and writer proxy data objects, besides the builtin proxies are clear
    pdata->clear();

    std::lock_guard<std::recursive_mutex> lock(*getMutex());
    // Return proxy object to pool
    participant_proxies_pool_.push_back(pdata);

    return true;
 
}

const BuiltinAttributes& PDP::builtin_attributes() const
{
    return mp_builtin->m_att;
}

void PDP::assert_remote_participant_liveliness(
        const GuidPrefix_t& remote_guid)
{
    if(remote_guid == getRTPSParticipant()->getGuid().guidPrefix)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> guardPDP(*mp_mutex);

    if(participant_proxies_.size() > 1)
    {

        for(ParticipantProxy* pp : participant_proxies_)
        {
            if(pp->get_guid_prefix() == remote_guid)
            {
                pp->assert_liveliness();
                break;
            }
        }
    }
}

CDRMessage_t PDP::get_participant_proxy_data_serialized(Endianness_t endian)
{
    std::lock_guard<std::recursive_mutex> guardPDP(*this->mp_mutex);
    CDRMessage_t cdr_msg;
    cdr_msg.msg_endian = endian;

    if (!getLocalParticipantProxy()->proxy_data_->writeToCDRMessage(&cdr_msg, false))
    {
        cdr_msg.pos = 0;
        cdr_msg.length = 0;
    }

    return cdr_msg;
}


void PDP::check_remote_participant_liveliness(
        ParticipantProxy* remote_participant)
{
    std::unique_lock<std::recursive_mutex> guard(*mp_mutex);

    if(GUID_t::unknown() != remote_participant->get_guid())
    {
        // Check last received message's time_point plus lease duration time doesn't overcome now().
        // If overcame, remove participant.
        auto now = std::chrono::steady_clock::now();
        auto real_lease_tm = remote_participant->last_received_message_tm() +
            remote_participant->proxy_data_->lease_duration_us_;

        assert(remote_participant->proxy_data_->lease_duration_us_ != std::chrono::microseconds());

        if (now > real_lease_tm)
        {
            guard.unlock();
            remove_remote_participant(remote_participant->get_guid(),
                ParticipantDiscoveryInfo::DROPPED_PARTICIPANT);
            return;
        }

        // Calculate next trigger.
        auto next_trigger = real_lease_tm - now;
        remote_participant->lease_duration_event_->update_interval_millisec(
                (double)std::chrono::duration_cast<std::chrono::milliseconds>(next_trigger).count());
        remote_participant->lease_duration_event_->restart_timer();
    }
}

void PDP::set_next_announcement_interval()
{
    if (initial_announcements_.count > 0)
    {
        --initial_announcements_.count;
        resend_participant_info_event_->update_interval(initial_announcements_.period);
    }
    else
    {
        resend_participant_info_event_->update_interval(m_discovery.discovery_config.leaseDuration_announcementperiod);
    }
}

void PDP::set_initial_announcement_interval()
{
    if ((initial_announcements_.count > 0) && (initial_announcements_.period <= c_TimeZero))
    {
        // Force a small interval (1ms) between initial announcements
        logWarning(RTPS_PDP, "Initial announcement period is not strictly positive. Changing to 1ms.");
        initial_announcements_.period = { 0, 1000000 };
    }
    set_next_announcement_interval();
}

// TODO: Iker. Participant allocation attributes SHOULD be moved to the library attributes in the future if we
// share the discovery data.

/*static*/
void PDP::initialize_or_update_pool_allocation(const RTPSParticipantAllocationAttributes& allocation)
{
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    participant_proxies_data_pool_.reserve(allocation.participants.initial);

    if( participant_proxies_data_number_ < allocation.participants.initial )
    {
        for (size_t i = participant_proxies_data_number_ ; i < allocation.participants.initial; ++i)
        {
            participant_proxies_data_pool_.push_back(new ParticipantProxyData(allocation));
        }

        participant_proxies_data_number_ = allocation.participants.initial;
    }

    // If max_unicast or max_multicast locators changes from participant config to participant config
    // then Reader and Writer proxies will end up with different allocated storage. See Iker's TODO above.
    size_t max_unicast_locators = allocation.locators.max_unicast_locators;
    size_t max_multicast_locators = allocation.locators.max_multicast_locators;

    reader_proxies_pool_.reserve(allocation.total_readers().initial);

    if( reader_proxies_number_ < allocation.total_readers().initial )
    {
        for (size_t i = reader_proxies_number_ ; i < allocation.total_readers().initial; ++i)
        {
            reader_proxies_pool_.push_back(new ReaderProxyData(max_unicast_locators, max_multicast_locators));
        }

        reader_proxies_number_ = allocation.total_readers().initial; 
    }

    writer_proxies_pool_.reserve(allocation.total_writers().initial);

    if( writer_proxies_number_ < allocation.total_writers().initial )
    {
        for (size_t i = writer_proxies_number_ ; i < allocation.total_writers().initial; ++i)
        {
            writer_proxies_pool_.push_back(new WriterProxyData(max_unicast_locators, max_multicast_locators));
        }
        
        writer_proxies_number_ = allocation.total_writers().initial;
    }
}

/*static*/
void PDP::remove_pool_resources()
{
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    if(!--pdp_counter_)
    {
        assert(pool_participant_references_.empty());

        for(ParticipantProxyData* it : participant_proxies_data_pool_)
        {
            delete it;
        }

        for(ReaderProxyData* it : reader_proxies_pool_)
        {
            delete it;
        }

        for(WriterProxyData* it : writer_proxies_pool_)
        {
            delete it;
        }


    }
}

/*static*/
std::shared_ptr<ParticipantProxyData> PDP::get_alived_participant_proxy(const GuidPrefix_t & guid)
{
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    auto it = pool_participant_references_.find(guid);

    if(it == pool_participant_references_.end())
    {
        // nothing there
        return std::shared_ptr<ParticipantProxyData>();
    }

    // recreate shared_ptr from weak
    return it->second.lock();
}

//!Get ReaderProxyData from the pool if there, nullptr otherwise
/*static*/
std::shared_ptr<ReaderProxyData> PDP::get_alived_reader_proxy(const GUID_t & guid)
{
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    auto it = pool_reader_references_.find(guid);

    if(it == pool_reader_references_.end())
    {
        // nothing there
        return std::shared_ptr<ReaderProxyData>();
    }

    // recreate shared_ptr from weak
    return it->second.lock();
}

//!Get WriterProxyData from the pool if there, nullptr otherwise
/*static*/
std::shared_ptr<WriterProxyData> PDP::get_alived_writer_proxy(const GUID_t & guid)
{
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    auto it = pool_writer_references_.find(guid);

    if(it == pool_writer_references_.end())
    {
        // nothing there
        return std::shared_ptr<WriterProxyData>();
    }

    // recreate shared_ptr from weak
    return it->second.lock();
}

/*static*/ 
std::shared_ptr<ReaderProxyData> PDP::get_from_reader_proxy_pool(
    const GUID_t & guid,
    const size_t max_unicast_locators,
    const size_t max_multicast_locators)
{
    // first assess that is not alived
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    std::shared_ptr<ReaderProxyData> ret_val = get_alived_reader_proxy(guid);
    bool add_reference = false;

    if(!ret_val)
    {
        add_reference = true;

        // Newbie, try to take one entry from the pool
        if(reader_proxies_pool_.empty())
        {
            size_t max_proxies = reader_proxies_pool_.max_size();
            if(reader_proxies_number_ < max_proxies)
            {
                // Pool is empty but limit has not been reached, so we create a new entry.
                ret_val = std::shared_ptr<ReaderProxyData>(new ReaderProxyData(
                    max_unicast_locators,
                    max_multicast_locators),
                    ReaderProxyData::pool_deleter());
                ++reader_proxies_number_;
            }
            else
            {
                logWarning(RTPS_PDP, "Maximum number of reader proxies (" << max_proxies <<
                    ") reached  " << std::endl);
            }
        }
        else
        {
            // Pool is not empty, use entry from pool
            ret_val.reset(reader_proxies_pool_.back(), ReaderProxyData::pool_deleter());
            reader_proxies_pool_.pop_back();
        }
    }

    // add reference if needed
    if(ret_val && add_reference)
    {
        pool_reader_references_[guid] = ret_val;
    }

    return ret_val;
}

/*static*/
std::shared_ptr<WriterProxyData> PDP::get_from_writer_proxy_pool(
    const GUID_t & guid,
    const size_t max_unicast_locators,
    const size_t max_multicast_locators)
{
    // first assess that is not alived
    std::lock_guard<std::recursive_mutex> lock(pool_mutex_);

    std::shared_ptr<WriterProxyData> ret_val = get_alived_writer_proxy(guid);
    bool add_reference = false;

    if(!ret_val)
    {
        // Newbie, try to take one entry from the pool
        add_reference = true;

        if(writer_proxies_pool_.empty())
        {
            size_t max_proxies = writer_proxies_pool_.max_size();
            if(writer_proxies_number_ < max_proxies)
            {
                // Pool is empty but limit has not been reached, so we create a new entry.
                ret_val = std::shared_ptr<WriterProxyData>(new WriterProxyData(
                    max_unicast_locators,
                    max_multicast_locators),
                    WriterProxyData::pool_deleter());
                ++writer_proxies_number_;
            }
            else
            {
                logWarning(RTPS_PDP, "Maximum number of writer proxies (" << max_proxies <<
                    ") reached" << std::endl);
            }
        }
        else
        {
            // Pool is not empty, use entry from pool
            ret_val.reset(writer_proxies_pool_.back(), WriterProxyData::pool_deleter());
            writer_proxies_pool_.pop_back();
        }
    }

    // add reference if needed
    if(ret_val && add_reference)
    {
        pool_writer_references_[guid] = ret_val;
    }

    return ret_val;
}

std::shared_ptr<ParticipantProxyData> PDP::get_from_local_proxies(const GuidPrefix_t & guid)
{
    std::lock_guard<std::recursive_mutex> lock(*mp_mutex);

    for(ParticipantProxy* p : participant_proxies_)
    {
        if(guid == p->get_guid_prefix())
        {
            return p->proxy_data_;
        }
    }

    // nothing there
    return std::shared_ptr<ParticipantProxyData>();
}

/*static*/ 
void PDP::return_participant_proxy_to_pool(ParticipantProxyData * p)
{
    assert(p != nullptr);

    GUID_t guid;

    {
        std::lock_guard<std::recursive_mutex> lock(p->ppd_mutex_);
        guid = p->m_guid;
        p->clear();
    }

    if( guid != c_Guid_Unknown)
    {
        std::lock_guard<std::recursive_mutex> lock(PDP::pool_mutex_);

        // if its a pool managed object should be included into the map
        assert(PDP::pool_participant_references_.find(guid.guidPrefix)
            != PDP::pool_participant_references_.end());

        PDP::pool_participant_references_.erase(guid.guidPrefix);
        PDP::participant_proxies_data_pool_.push_back(p);
    }
}

/*static*/
void PDP::return_reader_proxy_to_pool(ReaderProxyData * p)
{
    assert(p != nullptr);

    GUID_t guid;

    {
        auto lock = p->unique_lock();
        guid = p->guid();
        p->clear();
    }

    if(guid != c_Guid_Unknown)
    {
        std::lock_guard<std::recursive_mutex> lock(PDP::pool_mutex_);

        // if its a pool managed object should be included into the map
        assert(PDP::pool_reader_references_.find(guid) != PDP::pool_reader_references_.end());

        PDP::pool_reader_references_.erase(guid);
        PDP::reader_proxies_pool_.push_back(p);
    }
}

/*static*/
void PDP::return_writer_proxy_to_pool(WriterProxyData * p)
{
    assert(p != nullptr);

    GUID_t guid;

    {
        auto lock = p->unique_lock();
        guid = p->guid();
        p->clear();
    }

    if(guid != c_Guid_Unknown)
    {
        std::lock_guard<std::recursive_mutex> lock(PDP::pool_mutex_);

        // if its a pool managed object should be included into the map
        assert(PDP::pool_writer_references_.find(guid) != PDP::pool_writer_references_.end());

        PDP::pool_writer_references_.erase(guid);
        PDP::writer_proxies_pool_.push_back(p);
    }
}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
