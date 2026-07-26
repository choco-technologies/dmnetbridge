#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmnetbridge.h"

static dmnetbridge_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmnetbridge_create();
}

void dmod_test_teardown(void)
{
    dmnetbridge_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmnetbridge_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmnetbridge_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmnetbridge_is_valid(g_handle));
}

DMOD_TEST_STEP(dmnetbridge_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmnetbridge_destroy(NULL);
}
