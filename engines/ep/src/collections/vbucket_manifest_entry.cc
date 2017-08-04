/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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

#include "collections/vbucket_manifest_entry.h"

void Collections::VB::ManifestEntry::throwInvalidArg(
        const std::string& prefix) {
    std::stringstream ss;
    ss << " " << *this;
    throw std::invalid_argument(prefix + ss.str());
}

std::ostream& Collections::VB::operator<<(
        std::ostream& os,
        const Collections::VB::ManifestEntry& manifestEntry) {
    os << "ManifestEntry: collection:" << manifestEntry.getCollectionName()
       << ", uid:" << manifestEntry.getUid()
       << ", startSeqno:" << manifestEntry.getStartSeqno()
       << ", endSeqno:" << manifestEntry.getEndSeqno();
    return os;
}