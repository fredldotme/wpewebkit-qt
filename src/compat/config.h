#pragma once

#include <cmakeconfig.h>

#include <wtf/Platform.h>

#define RELEASE_ASSERT(x)
#define RELEASE_ASSERT_WITH_MESSAGE(x, y)
#define RELEASE_ASSERT_NOT_REACHED()

#define ENABLE(WTF_FEATURE) (defined ENABLE_##WTF_FEATURE && ENABLE_##WTF_FEATURE)

#define USE_GLIB 1
