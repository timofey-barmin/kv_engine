/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
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

#include <daemon/mcbp.h>
#include "executors.h"

#include <set>

// We can't use a set of enums that easily in an unordered_set.. just use an
// ordered for now..
using FeatureSet = std::set<cb::mcbp::Feature>;

/**
 * Try to see if the provided vector of features contais a certain feature
 *
 * @param features The vector to search
 * @param feature The feature to check for
 * @return true if it contains the feature, false otherwise
 */
bool containsFeature(const FeatureSet& features, cb::mcbp::Feature feature) {
    return features.find(feature) != features.end();
}

/**
 * Convert the input array of requested features into the Feature set which
 * don't include any illegal / unsupported features or any duplicates.
 *
 * In addition to that we'll also make sure that all dependent features is
 * enabled (and that we don't request features which are mutually exclusive)
 *
 * @param requested The set to populate with the requested features
 * @param input The input array
 */
void buildRequestVector(FeatureSet& requested, cb::sized_buffer<const uint16_t> input) {
    for (const auto& value : input) {
        const uint16_t in = ntohs(value);
        const auto feature = cb::mcbp::Feature(in);

        switch (feature) {
        case cb::mcbp::Feature::Invalid:
        case cb::mcbp::Feature::TLS:
            // known, but we don't support them
            break;
        case cb::mcbp::Feature::TCPNODELAY:
        case cb::mcbp::Feature::TCPDELAY:
        case cb::mcbp::Feature::MUTATION_SEQNO:
        case cb::mcbp::Feature::XATTR:
        case cb::mcbp::Feature::JSON:
        case cb::mcbp::Feature::SNAPPY:
        case cb::mcbp::Feature::XERROR:
        case cb::mcbp::Feature::SELECT_BUCKET:
        case cb::mcbp::Feature::COLLECTIONS:
        case cb::mcbp::Feature::Duplex:
        case cb::mcbp::Feature::ClustermapChangeNotification:
        case cb::mcbp::Feature::UnorderedExecution:
        case cb::mcbp::Feature::Tracing:

            // This isn't very optimal, but we've only got a handfull of elements ;)
            if (!containsFeature(requested, feature)) {
                requested.insert(feature);
            }

            break;
        }
    }

    // Run through the requested array and make sure we don't have
    // illegal combinations
    for (const auto& feature : requested) {
        switch (cb::mcbp::Feature(feature)) {
        case cb::mcbp::Feature::Invalid:
        case cb::mcbp::Feature::TLS:
        case cb::mcbp::Feature::MUTATION_SEQNO:
        case cb::mcbp::Feature::XATTR:
        case cb::mcbp::Feature::XERROR:
        case cb::mcbp::Feature::SELECT_BUCKET:
        case cb::mcbp::Feature::COLLECTIONS:
        case cb::mcbp::Feature::SNAPPY:
        case cb::mcbp::Feature::JSON:
        case cb::mcbp::Feature::Tracing:
        case cb::mcbp::Feature::Duplex:
        case cb::mcbp::Feature::UnorderedExecution:
            // No other dependency
            break;

        case cb::mcbp::Feature::TCPNODELAY:
            // cannot co-exist with TCPDELAY
            if (containsFeature(requested, cb::mcbp::Feature::TCPDELAY)) {
                throw std::invalid_argument("TCPNODELAY cannot co-exist with TCPDELAY");
            }
            break;
        case cb::mcbp::Feature::TCPDELAY:
            // cannot co-exist with TCPNODELAY
            if (containsFeature(requested, cb::mcbp::Feature::TCPNODELAY)) {
                throw std::invalid_argument("TCPDELAY cannot co-exist with TCPNODELAY");
            }
            break;
        case cb::mcbp::Feature::ClustermapChangeNotification:
            // Needs duplex
            if (!containsFeature(requested, cb::mcbp::Feature::Duplex)) {
                throw std::invalid_argument("ClustermapChangeNotification needs Duplex");
            }
            break;
        }
    }
}

void process_hello_packet_executor(Cookie& cookie) {
    auto& connection = cookie.getConnection();
    auto* req = reinterpret_cast<protocol_binary_request_hello*>(
            cookie.getPacketAsVoidPtr());
    std::string log_buffer;
    log_buffer.reserve(512);
    log_buffer.append("HELO ");

    const cb::const_char_buffer key{
        reinterpret_cast<const char*>(req->bytes + sizeof(req->bytes)),
        ntohs(req->message.header.request.keylen)};

    const cb::sized_buffer<const uint16_t> input{
        reinterpret_cast<const uint16_t*>(key.data() + key.size()),
        (ntohl(req->message.header.request.bodylen) - key.size()) / 2};

    std::vector<uint16_t> out;

    // We can't switch bucket if we've got multiple commands in flight
    if (connection.getNumberOfCookies() > 1) {
        LOG_NOTICE(&connection,
                   "%u: %s Changing options via HELO is not possible with "
                   "multiple "
                   "commands in flight",
                   connection.getId(),
                   connection.getDescription().c_str());
        cookie.sendResponse(cb::mcbp::Status::NotSupported);
        return;
    }

    FeatureSet requested;
    try {
        buildRequestVector(requested, input);
    } catch (const std::invalid_argument& e) {
        LOG_NOTICE(&connection,
                   "%u: %s Invalid combination of options: %s",
                   connection.getId(),
                   connection.getDescription().c_str(),
                   e.what());
        cookie.setErrorContext(e.what());
        cookie.sendResponse(cb::mcbp::Status::Einval);
        return;
    }

    /*
     * Disable all features the hello packet may enable, so that
     * the client can toggle features on/off during a connection
     */
    connection.disableAllDatatypes();
    connection.setSupportsMutationExtras(false);
    connection.setXerrorSupport(false);
    connection.setCollectionsSupported(false);
    connection.setDuplexSupported(false);
    connection.setClustermapChangeNotificationSupported(false);
    connection.setAgentName(key);
    connection.setTracingEnabled(false);
    connection.setAllowUnorderedExecution(false);

    if (!key.empty()) {
        log_buffer.append("[");
        if (key.size() > 256) {
            log_buffer.append(key.data(), 256);
            log_buffer.append("...");
        } else {
            log_buffer.append(key.data(), key.size());
        }
        log_buffer.append("] ");
    }

    for (const auto& feature : requested) {
        bool added = false;

        switch (feature) {
        case cb::mcbp::Feature::Invalid:
        case cb::mcbp::Feature::TLS:
            // Not implemented
            LOG_NOTICE(nullptr,
                       "%u: %s requested unupported feature %s",
                       connection.getId(),
                       connection.getDescription().c_str(),
                       to_string(feature).c_str());
            break;
        case cb::mcbp::Feature::TCPNODELAY:
        case cb::mcbp::Feature::TCPDELAY:
            connection.setTcpNoDelay(feature == cb::mcbp::Feature::TCPNODELAY);
            added = true;
            break;

        case cb::mcbp::Feature::MUTATION_SEQNO:
            connection.setSupportsMutationExtras(true);
            added = true;
            break;
        case cb::mcbp::Feature::XATTR:
            if ((Datatype::isSupported(cb::mcbp::Feature::XATTR) ||
                 connection.isInternal())) {
                connection.enableDatatype(cb::mcbp::Feature::XATTR);
                added = true;
            }
            break;
        case cb::mcbp::Feature::JSON:
            if (Datatype::isSupported(cb::mcbp::Feature::JSON)) {
                connection.enableDatatype(cb::mcbp::Feature::JSON);
                added = true;
            }
            break;
        case cb::mcbp::Feature::SNAPPY:
            if (Datatype::isSupported(cb::mcbp::Feature::SNAPPY)) {
                connection.enableDatatype(cb::mcbp::Feature::SNAPPY);
                added = true;
            }
            break;
        case cb::mcbp::Feature::XERROR:
            connection.setXerrorSupport(true);
            added = true;
            break;
        case cb::mcbp::Feature::SELECT_BUCKET:
            // The select bucket is only informative ;-)
            added = true;
            break;
        case cb::mcbp::Feature::COLLECTIONS:
            connection.setCollectionsSupported(true);
            added = true;
            break;
        case cb::mcbp::Feature::Duplex:
            connection.setDuplexSupported(true);
            added = true;
            break;
        case cb::mcbp::Feature::ClustermapChangeNotification:
            connection.setClustermapChangeNotificationSupported(true);
            added = true;
            break;
        case cb::mcbp::Feature::UnorderedExecution:
            if (connection.isDCP()) {
                LOG_NOTICE(&connection,
                           "%u: %s Unordered execution is not supported for "
                           "DCP connections",
                           connection.getId(),
                           connection.getDescription().c_str());
            } else {
                connection.setAllowUnorderedExecution(true);
                added = true;
            }
            break;

        case cb::mcbp::Feature::Tracing:
            if (settings.isTracingEnabled()) {
                connection.setTracingEnabled(true);
                added = true;
                break;
            } else {
                LOG_NOTICE(&connection,
                           "%u: %s Request for [disabled] Tracing feature",
                           connection.getId(),
                           connection.getDescription().c_str());
            }

        } // end switch

        if (added) {
            out.push_back(htons(uint16_t(feature)));
            log_buffer.append(to_string(feature));
            log_buffer.append(", ");
        }
    }

    cookie.sendResponse(
            cb::mcbp::Status::Success,
            {},
            {},
            {reinterpret_cast<const char*>(out.data()), 2 * out.size()},
            cb::mcbp::Datatype::Raw,
            0);

    // Trim off the trailing whitespace (and potentially comma)
    log_buffer.resize(log_buffer.size() - 1);
    if (log_buffer.back() == ',') {
        log_buffer.resize(log_buffer.size() - 1);
    }

    LOG_NOTICE(&connection,
               "%u: %s %s",
               connection.getId(),
               log_buffer.c_str(),
               connection.getDescription().c_str());
}
