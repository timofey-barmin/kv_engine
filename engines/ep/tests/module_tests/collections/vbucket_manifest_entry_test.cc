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

#include <gtest/gtest.h>

// Basic ManifestEntry construction checks
TEST(ManifestEntry, test_getters) {
    std::string collection = "beer";
    Collections::VB::ManifestEntry m({collection, 100},
                                     1000, // start
                                     StoredValue::state_collection_open); // end
    EXPECT_EQ(1000, m.getStartSeqno());
    EXPECT_EQ(100, m.getUid());
    EXPECT_EQ(StoredValue::state_collection_open, m.getEndSeqno());
    EXPECT_TRUE(m.isOpen());
    EXPECT_EQ("beer", m.getCollectionName());
    EXPECT_EQ(strlen("beer"), m.getCharBuffer().size());
    EXPECT_STREQ("beer", m.getCharBuffer().data());
}

// Check isDeleting changes state when end seqno is adjusted
TEST(ManifestEntry, test_state) {
    std::string collection = "beer";

    // Collection starts at seqno 1000
    Collections::VB::ManifestEntry m({collection, 1},
                                     1000, // start
                                     StoredValue::state_collection_open); // end
    EXPECT_TRUE(m.isOpen());
    EXPECT_FALSE(m.isDeleting());

    // Deleted at seqno 2000
    m.setEndSeqno(2000);
    EXPECT_TRUE(m.isDeleting());
    EXPECT_FALSE(m.isOpen());

    // Re-added at seqno 3000
    m.setStartSeqno(3000);
    EXPECT_TRUE(m.isOpen());
    EXPECT_TRUE(m.isDeleting());

    // Delete completed
    m.resetEndSeqno();
    EXPECT_FALSE(m.isDeleting());
    EXPECT_TRUE(m.isOpen());
}

TEST(ManifestEntry, exceptions) {
    std::string collection = "beer";
    cb::const_char_buffer buf(collection);

    // Collection starts at seqno 1000
    Collections::VB::ManifestEntry m({collection, 1},
                                     1000, // start
                                     StoredValue::state_collection_open); // end

    // set end so it's not StoredValue::state_collection_open for full set of
    // start checks
    m.setEndSeqno(2000);

    // Now start = 1000 and end = 2000
    // Check we cannot change start to be...
    EXPECT_THROW(m.setStartSeqno(999), std::invalid_argument); // ... smaller
    EXPECT_THROW(m.setStartSeqno(1000), std::invalid_argument); // ... the same
    EXPECT_THROW(m.setStartSeqno(-1), std::invalid_argument); // ... negative
    EXPECT_THROW(m.setStartSeqno(2000), std::invalid_argument); // ... end
    EXPECT_THROW(m.setStartSeqno(StoredValue::state_collection_open),
                 std::invalid_argument); // note this test same as negative.

    EXPECT_NO_THROW(m.setStartSeqno(3000));

    // Now start = 3000 and end = 2000
    // Check we cannot change end to be...
    EXPECT_THROW(m.setEndSeqno(2000), std::invalid_argument); // ... the same
    EXPECT_THROW(m.setEndSeqno(1999), std::invalid_argument); // ... smaller
    EXPECT_THROW(m.setEndSeqno(3000), std::invalid_argument); // ... start
    // ... not this
    EXPECT_THROW(m.setEndSeqno(StoredValue::state_deleted_key),
                 std::invalid_argument);
    // the only negative value allowed should be state_collection_open
    EXPECT_NO_THROW(m.setEndSeqno(StoredValue::state_collection_open));

    EXPECT_THROW(Collections::VB::ManifestEntry({collection, 1}, 100, 100),
                 std::invalid_argument);
}

TEST(ManifestEntry, construct_assign) {
    std::string collection = "beer";

    // Collection starts at seqno 1000
    Collections::VB::ManifestEntry entry1({collection, 5}, 2, 9);

    //  Move entry1 to entry2
    Collections::VB::ManifestEntry entry2(std::move(entry1));
    EXPECT_EQ(5, entry2.getUid());
    EXPECT_EQ(2, entry2.getStartSeqno());
    EXPECT_EQ(9, entry2.getEndSeqno());
    EXPECT_STREQ("beer", entry2.getCollectionName().c_str());

    // Take a copy of entry2
    Collections::VB::ManifestEntry entry3(entry2);
    EXPECT_EQ(5, entry3.getUid());
    EXPECT_EQ(2, entry3.getStartSeqno());
    EXPECT_EQ(9, entry3.getEndSeqno());
    EXPECT_STREQ("beer", entry3.getCollectionName().c_str());

    // change entry2
    entry2.setUid(6);
    entry2.setEndSeqno(10);
    entry2.setStartSeqno(3);

    // Now copy entry2 assign over entry3
    entry3 = entry2;
    EXPECT_EQ(6, entry3.getUid());
    EXPECT_EQ(3, entry3.getStartSeqno());
    EXPECT_EQ(10, entry3.getEndSeqno());
    EXPECT_STREQ("beer", entry3.getCollectionName().c_str());

    // And move entry3 back to entry1
    entry1 = std::move(entry3);

    EXPECT_EQ(6, entry1.getUid());
    EXPECT_EQ(3, entry1.getStartSeqno());
    EXPECT_EQ(10, entry1.getEndSeqno());
    EXPECT_STREQ("beer", entry1.getCollectionName().c_str());
}