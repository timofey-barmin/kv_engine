/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "engine_errc_2_mcbp.h"

protocol_binary_response_status mcbp::to_status(cb::engine_errc code) {
    using namespace cb;

    switch (code) {
    case engine_errc::no_access:
        return PROTOCOL_BINARY_RESPONSE_EACCESS;
    case engine_errc::success:
        return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    case engine_errc::no_such_key:
        return PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
    case engine_errc::key_already_exists:
        return PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    case engine_errc::no_memory:
        return PROTOCOL_BINARY_RESPONSE_ENOMEM;
    case engine_errc::temporary_failure:
        return PROTOCOL_BINARY_RESPONSE_ETMPFAIL;
    case engine_errc::not_stored:
        return PROTOCOL_BINARY_RESPONSE_NOT_STORED;
    case engine_errc::invalid_arguments:
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    case engine_errc::not_supported:
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    case engine_errc::too_big:
        return PROTOCOL_BINARY_RESPONSE_E2BIG;
    case engine_errc::not_my_vbucket:
        return PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET;
    case engine_errc::out_of_range:
        return PROTOCOL_BINARY_RESPONSE_ERANGE;
    case engine_errc::rollback:
        return PROTOCOL_BINARY_RESPONSE_ROLLBACK;
    case engine_errc::no_bucket:
        return PROTOCOL_BINARY_RESPONSE_NO_BUCKET;
    case engine_errc::too_busy:
        return PROTOCOL_BINARY_RESPONSE_EBUSY;
    case engine_errc::authentication_stale:
        return PROTOCOL_BINARY_RESPONSE_AUTH_STALE;
    case engine_errc::delta_badval:
        return PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL;

    case engine_errc::would_block:
        throw std::logic_error(
            "mcbp::to_status: would_block is not a legal error code to send to the user");

    case engine_errc::want_more:
        throw std::logic_error(
            "mcbp::to_status: want_more is not a legal error code to send to the user");

    case engine_errc::disconnect:
        throw std::logic_error(
            "mcbp::to_status: disconnect is not a legal error code to send to the user");

    case engine_errc::locked:
    case engine_errc::locked_tmpfail:
        return PROTOCOL_BINARY_RESPONSE_LOCKED;

    case engine_errc::unknown_collection:
        return PROTOCOL_BINARY_RESPONSE_UNKNOWN_COLLECTION;
    case engine_errc::no_collections_manifest:
        return PROTOCOL_BINARY_RESPONSE_NO_COLLECTIONS_MANIFEST;
    case engine_errc::predicate_failed:
        throw std::logic_error(
                "mcbp::to_status: predicate_failed is not a legal error code "
                "to send to the user");
    case engine_errc::failed:
        return PROTOCOL_BINARY_RESPONSE_EINTERNAL;
    }

    throw std::invalid_argument(
        "mcbp::to_status: Invalid argument " + std::to_string(int(code)));
}

cb::mcbp::Status cb::mcbp::to_status(cb::engine_errc code) {
    using namespace cb;
    using namespace cb::mcbp;

    switch (code) {
    case engine_errc::no_access:
        return Status::Eaccess;
    case engine_errc::success:
        return Status::Success;
    case engine_errc::no_such_key:
        return Status::KeyEnoent;
    case engine_errc::key_already_exists:
        return Status::KeyEexists;
    case engine_errc::no_memory:
        return Status::Enomem;
    case engine_errc::temporary_failure:
        return Status::Etmpfail;
    case engine_errc::not_stored:
        return Status::NotStored;
    case engine_errc::invalid_arguments:
        return Status::Einval;
    case engine_errc::not_supported:
        return Status::NotSupported;
    case engine_errc::too_big:
        return Status::E2big;
    case engine_errc::not_my_vbucket:
        return Status::NotMyVbucket;
    case engine_errc::out_of_range:
        return Status::Erange;
    case engine_errc::rollback:
        return Status::Rollback;
    case engine_errc::no_bucket:
        return Status::NoBucket;
    case engine_errc::too_busy:
        return Status::Ebusy;
    case engine_errc::authentication_stale:
        return Status::AuthStale;
    case engine_errc::delta_badval:
        return Status::DeltaBadval;

    case engine_errc::would_block:
        throw std::logic_error(
                "mcbp::to_status: would_block is not a legal error code to "
                "send to the user");

    case engine_errc::want_more:
        throw std::logic_error(
                "mcbp::to_status: want_more is not a legal error code to send "
                "to the user");

    case engine_errc::disconnect:
        throw std::logic_error(
                "mcbp::to_status: disconnect is not a legal error code to send "
                "to the user");

    case engine_errc::locked:
    case engine_errc::locked_tmpfail:
        return Status::Locked;

    case engine_errc::unknown_collection:
        return Status::UnknownCollection;
    case engine_errc::no_collections_manifest:
        return Status::NoCollectionsManifest;
    case engine_errc::predicate_failed:
        throw std::logic_error(
                "mcbp::to_status: predicate_failed is not a legal error code "
                "to send to the user");
    case engine_errc::failed:
        return Status::Einternal;
    }

    throw std::invalid_argument("mcbp::to_status: Invalid argument " +
                                std::to_string(int(code)));
}