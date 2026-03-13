#include <unity.h>
#include "protocol/iohome_rolling_code_store.h"
#include <string.h>

void test_memory_store_save_load(void) {
    iohome::MemoryRollingCodeStore store;
    uint8_t node[3] = {0x1A, 0x38, 0x0B};

    TEST_ASSERT_TRUE(store.save(node, 42));

    uint16_t code = 0;
    TEST_ASSERT_TRUE(store.load(node, code));
    TEST_ASSERT_EQUAL_UINT16(42, code);
}

void test_memory_store_load_not_found(void) {
    iohome::MemoryRollingCodeStore store;
    uint8_t node[3] = {0xFF, 0xFF, 0xFF};

    uint16_t code = 999;
    TEST_ASSERT_FALSE(store.load(node, code));
    TEST_ASSERT_EQUAL_UINT16(0, code);
}

void test_memory_store_overwrite(void) {
    iohome::MemoryRollingCodeStore store;
    uint8_t node[3] = {0x1A, 0x38, 0x0B};

    store.save(node, 100);
    store.save(node, 200);

    uint16_t code = 0;
    store.load(node, code);
    TEST_ASSERT_EQUAL_UINT16(200, code);
}

void test_memory_store_multiple_nodes(void) {
    iohome::MemoryRollingCodeStore store;
    uint8_t node1[3] = {0x01, 0x02, 0x03};
    uint8_t node2[3] = {0x04, 0x05, 0x06};

    store.save(node1, 111);
    store.save(node2, 222);

    uint16_t code1 = 0, code2 = 0;
    store.load(node1, code1);
    store.load(node2, code2);

    TEST_ASSERT_EQUAL_UINT16(111, code1);
    TEST_ASSERT_EQUAL_UINT16(222, code2);
}

void test_memory_store_max_entries(void) {
    iohome::MemoryRollingCodeStore store;

    // Fill all 16 slots
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t node[3] = {i, 0x00, 0x00};
        TEST_ASSERT_TRUE(store.save(node, i * 10));
    }

    // 17th should fail
    uint8_t overflow_node[3] = {0xFF, 0xFF, 0xFF};
    TEST_ASSERT_FALSE(store.save(overflow_node, 999));

    // Existing entries should still be readable
    uint8_t node0[3] = {0x00, 0x00, 0x00};
    uint16_t code = 0;
    TEST_ASSERT_TRUE(store.load(node0, code));
    TEST_ASSERT_EQUAL_UINT16(0, code);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_memory_store_save_load);
    RUN_TEST(test_memory_store_load_not_found);
    RUN_TEST(test_memory_store_overwrite);
    RUN_TEST(test_memory_store_multiple_nodes);
    RUN_TEST(test_memory_store_max_entries);

    return UNITY_END();
}
