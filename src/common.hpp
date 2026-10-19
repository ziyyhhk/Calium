#pragma once

#include <Geode/Geode.hpp>
#include <glm/glm.hpp>

using namespace geode::prelude;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using usize = size_t;
using f32 = float;
using f64 = double;

// #define CALIUM_ASSERT(COND) { if (!(COND)) geode::log::error("{}:{}: Assertion failed: {}", fs::path(__FILE__).filename(), __LINE__, #COND); }
#define CALIUM_ASSERT(COND)

// TODO: Figure out how to set usize to the pointer size of this machine's architecture
