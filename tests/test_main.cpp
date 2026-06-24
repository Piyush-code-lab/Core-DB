#define TESTING
#include "../src/main.cpp"
#include <gtest/gtest.h>

// ---------- PAGER TESTS ----------

TEST(PagerTest, CreatesNewFileIfNotExists) {
    remove("test_pager.db");
    Pager pager("test_pager.db");
    EXPECT_EQ(pager.file_length, 0);
}

TEST(PagerTest, GetPageReturnsZeroedPageForNewPage) {
    remove("test_pager2.db");
    Pager pager("test_pager2.db");
    char* page = pager.get_page(0);
    bool all_zero = true;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        if (page[i] != 0) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero);
}

TEST(PagerTest, WriteAndReadBackPersistsData) {
    remove("test_pager3.db");
    {
        Pager pager("test_pager3.db");
        char* page = pager.get_page(0);
        page[0] = 42;
        pager.flush(0);
    } // Pager destructed here, file closed

    Pager pager2("test_pager3.db");
    char* page2 = pager2.get_page(0);
    EXPECT_EQ((unsigned char)page2[0], 42);
}

TEST(PagerTest, EvictionRespectsBufferPoolLimit) {
    remove("test_pager4.db");
    Pager pager("test_pager4.db");
    // Load more pages than BUFFER_POOL_SIZE allows
    for (uint32_t i = 0; i < BUFFER_POOL_SIZE + 5; i++) {
        pager.get_page(i);
    }
    EXPECT_LE(pager.lru_list.size(), BUFFER_POOL_SIZE);
}

// ---------- LEAF NODE TESTS ----------

TEST(LeafNodeTest, InsertKeepsKeysSorted) {
    char node[PAGE_SIZE];
    initialize_leaf_node(node);

    Row r1 = {3, "c", "c@test.com"};
    Row r2 = {1, "a", "a@test.com"};
    Row r3 = {2, "b", "b@test.com"};

    leaf_node_insert(node, 3, r1);
    leaf_node_insert(node, 1, r2);
    leaf_node_insert(node, 2, r3);

    EXPECT_EQ(*leaf_node_key(node, 0), 1);
    EXPECT_EQ(*leaf_node_key(node, 1), 2);
    EXPECT_EQ(*leaf_node_key(node, 2), 3);
}

TEST(LeafNodeTest, SearchFindsExistingKey) {
    char node[PAGE_SIZE];
    initialize_leaf_node(node);
    Row r = {5, "test", "test@test.com"};
    leaf_node_insert(node, 5, r);

    Row found;
    bool result = leaf_node_search(node, 5, &found);
    EXPECT_TRUE(result);
    EXPECT_STREQ(found.username, "test");
}

TEST(LeafNodeTest, SearchReturnsFalseForMissingKey) {
    char node[PAGE_SIZE];
    initialize_leaf_node(node);
    Row found;
    bool result = leaf_node_search(node, 999, &found);
    EXPECT_FALSE(result);
}

TEST(LeafNodeTest, SplitProducesTwoSortedHalvesWithNoOverlap) {
    char old_node[PAGE_SIZE];
    char new_node[PAGE_SIZE];
    initialize_leaf_node(old_node);

    for (uint32_t i = 0; i < LEAF_NODE_MAX_CELLS; i++) {
        Row r = {i, "u", "u@test.com"};
        leaf_node_insert(old_node, i, r);
    }

    Row overflow = {9999, "overflow", "overflow@test.com"};
    leaf_node_split_and_insert(old_node, new_node, 9999, overflow);

    uint32_t old_count = *leaf_node_num_cells(old_node);
    uint32_t new_count = *leaf_node_num_cells(new_node);

    EXPECT_EQ(old_count + new_count, LEAF_NODE_MAX_CELLS + 1);

    // Every key in old_node should be smaller than every key in new_node
    uint32_t max_old_key = *leaf_node_key(old_node, old_count - 1);
    uint32_t min_new_key = *leaf_node_key(new_node, 0);
    EXPECT_LT(max_old_key, min_new_key);
}

// ---------- INTERNAL NODE TESTS ----------

TEST(InternalNodeTest, FindChildRoutesCorrectly) {
    char node[PAGE_SIZE];
    initialize_internal_node(node);
    *internal_node_num_keys(node) = 1;
    *internal_node_key(node, 0) = 7;

    EXPECT_EQ(internal_node_find_child(node, 3), 0);   // < 7 -> child 0
    EXPECT_EQ(internal_node_find_child(node, 7), 1);   // >= 7 -> child 1
    EXPECT_EQ(internal_node_find_child(node, 100), 1); // >= 7 -> child 1
}

// ---------- TABLE INTEGRATION TESTS ----------

TEST(TableTest, InsertAndFindRoundTrip) {
    remove("test_table.db");
    remove("test_table.db.meta");
    Table table("test_table.db");

    Row r = {42, "piyush", "piyush@kgp.edu"};
    table.insert_row(42, r);

    Row found;
    bool result = table.find_row(42, &found);
    EXPECT_TRUE(result);
    EXPECT_STREQ(found.username, "piyush");
}

TEST(TableTest, InsertManyRowsTriggersSplitAndAllAreFindable) {
    remove("test_table2.db");
    remove("test_table2.db.meta");
    Table table("test_table2.db");

    for (uint32_t i = 1; i <= 30; i++) {
        Row r;
        r.id = i;
        string uname = "user" + to_string(i);
        strncpy(r.username, uname.c_str(), sizeof(r.username) - 1);
        r.username[sizeof(r.username) - 1] = '\0';
        table.insert_row(i, r);
    }

    for (uint32_t i = 1; i <= 30; i++) {
        Row found;
        bool result = table.find_row(i, &found);
        EXPECT_TRUE(result) << "Failed to find key " << i;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}