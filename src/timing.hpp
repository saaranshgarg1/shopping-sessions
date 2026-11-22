#pragma once
#ifndef SS_PROFILE
#define SS_PROFILE 0
#endif
#if SS_PROFILE
#define SS_LOG(statement) do { statement; } while (false)
#else
#define SS_LOG(statement) do {} while (false)
#endif
