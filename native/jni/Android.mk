LOCAL_PATH := $(call my-dir)/..

XFADE_CORE_SRC := src/status.cpp src/passthrough_pipeline.cpp src/fake_sl.cpp src/real_backend.cpp src/mixer.cpp src/boundary.cpp src/ring_pipeline.cpp src/dump.cpp

include $(CLEAR_VARS)
LOCAL_MODULE := xfade
LOCAL_SRC_FILES := $(XFADE_CORE_SRC) src/library.cpp src/xfade_jni.cpp
LOCAL_LDLIBS := -llog -ldl
# --exclude-libs hides symbols pulled from prebuilt archives (e.g. libc++_static.a) but not the vtable
# and typeinfo that libc++ headers instantiate directly into our own objects (e.g. std::make_shared's
# control block); libc++ marks those classes with an explicit default-visibility attribute that -fvisibility
# =hidden cannot override. The version script is the reliable way to cap the exported set at exactly the
# OpenSL and JNI entry points. Its version node is anonymous (no name before the '{'), so no VERDEF is
# emitted and the nine exports stay unversioned.
LOCAL_LDFLAGS := -Wl,--exclude-libs,ALL -Wl,--version-script=$(LOCAL_PATH)/jni/xfade.map
LOCAL_ADDITIONAL_DEPENDENCIES := $(LOCAL_PATH)/jni/xfade.map
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := xfade_tests
LOCAL_SRC_FILES := $(XFADE_CORE_SRC) test/test_main.cpp test/test_status.cpp test/test_fake_sl.cpp test/test_real_backend.cpp test/test_ring_mixer.cpp test/test_boundary.cpp test/test_ring_pipeline.cpp test/test_wiring.cpp
LOCAL_LDLIBS := -llog -ldl
LOCAL_CFLAGS := -DXFADE_TESTS
include $(BUILD_EXECUTABLE)
