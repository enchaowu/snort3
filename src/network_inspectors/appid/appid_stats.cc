//--------------------------------------------------------------------------
// Copyright (C) 2014-2020 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// appid_stats.cc author Sourcefire Inc.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "appid_stats.h"

#include "log/text_log.h"
#include "log/unified2.h"
#include "time/packet_time.h"

#include "appid_config.h"
#include "app_info_table.h"
#include "appid_session.h"

using namespace snort;

#define URLCATBUCKETS   100
#define URLREPBUCKETS   5

struct AppIdStatRecord
{
    char* app_name = nullptr;
    uint64_t initiatorBytes;
    uint64_t responderBytes;
};

static const char appid_stats_filename[] = "appid_stats.log";

static THREAD_LOCAL AppIdStatistics* appid_stats_manager = nullptr;

static void delete_record(void* record)
{
    snort_free(((AppIdStatRecord*)record)->app_name);
    snort_free(record);
}

void AppIdStatistics::end_stats_period()
{
    SF_LIST* bucketList = logBuckets;
    logBuckets = currBuckets;
    currBuckets = bucketList;
}

StatsBucket* AppIdStatistics::get_stats_bucket(time_t startTime)
{
    StatsBucket* bucket = nullptr;

    if ( !currBuckets )
        currBuckets = sflist_new();

    SF_LNODE* lNode = nullptr;
    StatsBucket* lBucket = nullptr;

    for ( lBucket = (StatsBucket*)sflist_first(currBuckets, &lNode); lNode && lBucket;
        lBucket = (StatsBucket*)sflist_next(&lNode) )
    {
        if (startTime == lBucket->startTime)
        {
            bucket = lBucket;
            break;
        }
        else if (startTime < lBucket->startTime)
        {
            bucket = (StatsBucket*)snort_calloc(sizeof(StatsBucket));
            bucket->startTime = startTime;
            bucket->appsTree = fwAvlInit();
            sflist_add_before(currBuckets, lNode, bucket);
            break;
        }
    }

    if ( !lNode )
    {
        bucket = (StatsBucket*)snort_calloc(sizeof(StatsBucket));
        bucket->startTime = startTime;
        bucket->appsTree = fwAvlInit();
        sflist_add_tail(currBuckets, bucket);
    }

    return bucket;
}

void AppIdStatistics::open_stats_log_file()
{
    log = TextLog_Init(appid_stats_filename, 4096, rollSize);
}

void AppIdStatistics::dump_statistics()
{
    if ( !logBuckets )
        return;

    if ( !log )
        open_stats_log_file();

    struct StatsBucket* bucket = nullptr;

    while ((bucket = (struct StatsBucket*)sflist_remove_head(logBuckets)) != nullptr)
    {
        if ( bucket->appRecordCnt )
        {
            struct FwAvlNode* node;

            for (node = fwAvlFirst(bucket->appsTree); node != nullptr; node = fwAvlNext(node))
            {
                struct AppIdStatRecord* record;

                record = (struct AppIdStatRecord*)node->data;

                // FIXIT-M %lu won't do time_t on 32-bit systems
                TextLog_Print(log, "%lu,%s," STDu64 "," STDu64 "\n",
                    packet_time(), record->app_name, record->initiatorBytes, record->responderBytes);
            }
        }
        fwAvlDeleteTree(bucket->appsTree, delete_record);
        snort_free(bucket);
    }
}

AppIdStatistics::AppIdStatistics(const AppIdConfig& config)
{
    enabled = true;

    rollSize = config.app_stats_rollover_size;
    bucketInterval = config.app_stats_period;

    time_t now = get_time();
    start_stats_period(now);
}

AppIdStatistics::~AppIdStatistics()
{
    if ( !enabled )
        return;

    /*flush the last stats period. */
    end_stats_period();
    dump_statistics();

    if ( log )
        TextLog_Term(log);

    if ( logBuckets )
        snort_free(logBuckets);

    if ( currBuckets )
    {
        while (auto bucket = (StatsBucket*)sflist_remove_head(currBuckets))
        {
            fwAvlDeleteTree(bucket->appsTree, delete_record);
            snort_free(bucket);
        }
        snort_free(currBuckets);
    }
}

AppIdStatistics* AppIdStatistics::initialize_manager(const AppIdConfig& config)
{
    if ( !config.log_stats )
        return nullptr;

    appid_stats_manager = new AppIdStatistics(config);
    return appid_stats_manager;
}

AppIdStatistics* AppIdStatistics::get_stats_manager()
{ return appid_stats_manager; }

void AppIdStatistics::cleanup()
{ delete appid_stats_manager; }

static void update_stats(const AppIdSession& asd, AppId app_id, StatsBucket* bucket)
{
    AppIdStatRecord* record = (AppIdStatRecord*)(fwAvlLookup(app_id, bucket->appsTree));
    if ( !record )
    {
        char tmp_buff[MAX_EVENT_APPNAME_LEN];
        bool cooked_client = false;

        record = (AppIdStatRecord*)(snort_calloc(sizeof(struct AppIdStatRecord)));

        if ( app_id >= 2000000000 )
            cooked_client = true;

        AppInfoTableEntry* entry
            = asd.ctxt.get_odp_ctxt().get_app_info_mgr().get_app_info_entry(app_id);

        if ( entry )
        {
            if (cooked_client)
            {
                snprintf(tmp_buff, MAX_EVENT_APPNAME_LEN, "_cl_%s", entry->app_name);
                tmp_buff[MAX_EVENT_APPNAME_LEN-1] = 0;
                record->app_name = snort_strdup(tmp_buff);
            }
            else
                record->app_name = snort_strdup(entry->app_name);
        }
        else if ( app_id == APP_ID_UNKNOWN || app_id == APP_ID_UNKNOWN_UI )
            record->app_name = snort_strdup("__unknown");
        else if ( app_id == APP_ID_NONE )
            record->app_name = snort_strdup("__none");
        else
        {
            if (cooked_client)
                snprintf(tmp_buff, MAX_EVENT_APPNAME_LEN, "_err_cl_%d",app_id);
            else
                snprintf(tmp_buff, MAX_EVENT_APPNAME_LEN, "_err_%d",app_id);

            tmp_buff[MAX_EVENT_APPNAME_LEN - 1] = 0;
            record->app_name = snort_strdup(tmp_buff);
        }

        if (fwAvlInsert(app_id, record, bucket->appsTree) == 0)
        {
            bucket->appRecordCnt += 1;
        }
        else
        {
            WarningMessage("Error saving statistics record for app id: %d", app_id);
            snort_free(record);
            record = nullptr;
        }
    }

    if ( record )
    {
        record->initiatorBytes += asd.stats.initiator_bytes;
        record->responderBytes += asd.stats.responder_bytes;
    }
}

void AppIdStatistics::update(const AppIdSession& asd)
{
    time_t now = get_time();

    if ( now >= bucketEnd )
    {
        end_stats_period();
        dump_statistics();
        start_stats_period(now);
    }

    time_t bucketTime = asd.stats.first_packet_second -
        (asd.stats.first_packet_second % bucketInterval);

    StatsBucket* bucket = get_stats_bucket(bucketTime);
    if ( !bucket )
        return;

    bucket->totalStats.txByteCnt += asd.stats.initiator_bytes;
    bucket->totalStats.rxByteCnt += asd.stats.responder_bytes;

    AppId web_app_id, service_id, client_id;
    asd.get_first_stream_app_ids(service_id, client_id, web_app_id);

    if ( web_app_id > APP_ID_NONE )
        update_stats(asd, web_app_id, bucket);

    if ( service_id && ( service_id != web_app_id ) )
        update_stats(asd, service_id, bucket);

    if ( client_id > APP_ID_NONE && client_id != service_id
        && client_id != web_app_id )
        update_stats(asd, client_id, bucket);
}

// Currently not registered to IdleProcessing
void AppIdStatistics::flush()
{
    if ( !enabled )
        return;

    time_t now = get_time();
    if (now >= bucketEnd)
    {
        end_stats_period();
        dump_statistics();
        start_stats_period(now);
    }
}

