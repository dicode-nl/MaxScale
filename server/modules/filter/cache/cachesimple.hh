/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-12-18
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <maxscale/ccdefs.hh>
#include <unordered_map>
#include "cache.hh"
#include "cache_storage_api.hh"

class Storage;

class CacheSimple : public Cache
{
public:
    ~CacheSimple();

    bool create_token(std::unique_ptr<Token>* psToken) override;

    cache_result_t get_value(Token* pToken,
                             const CACHE_KEY& key,
                             uint32_t flags,
                             uint32_t soft_ttl,
                             uint32_t hard_ttl,
                             GWBUF** ppValue,
                             std::function<void (cache_result_t, GWBUF*)> cb) const override final;

    cache_result_t put_value(Token* pToken,
                             const CACHE_KEY& key,
                             const std::vector<std::string>& invalidation_words,
                             const GWBUF* pValue,
                             std::function<void (cache_result_t)> cb) override final;

    cache_result_t del_value(Token* pToken,
                             const CACHE_KEY& key,
                             std::function<void (cache_result_t)> cb) override final;

    cache_result_t invalidate(Token* pToken,
                              const std::vector<std::string>& words) override final;

    cache_result_t clear(Token* pToken) override final;

protected:
    CacheSimple(const std::string& name,
                const CacheConfig* pConfig,
                const std::vector<SCacheRules>& Rules,
                SStorageFactory sFactory,
                Storage* pStorage);

    static bool create(const CacheConfig& config,
                       std::vector<SCacheRules>* pRules,
                       StorageFactory** ppFactory);


    json_t* do_get_info(uint32_t what) const;

    bool do_must_refresh(const CACHE_KEY& key, const CacheFilterSession* pSession);

    void do_refreshed(const CACHE_KEY& key, const CacheFilterSession* pSession);

private:
    CacheSimple(const Cache&);
    CacheSimple& operator=(const CacheSimple&);

protected:
    typedef std::unordered_map<CACHE_KEY, const CacheFilterSession*> Pending;

    Pending  m_pending; // Pending items; being fetched from the backend.
    Storage* m_pStorage;// The storage instance to use.
};
