#pragma once

// ---------------------------------------------------------------------------
// Runtime i18n (UI strings) — openvela/NuttX port.
// Ported from MetalioClaw4 main/i18n/i18n.h with NVS→file-based Settings.
// ---------------------------------------------------------------------------

#include "i18n_strings_gen.h"

#include <cstdarg>
#include <cstddef>

namespace I18n
{

void Init();

Locale GetLocale();
const char *GetLocaleCode();

bool SetLocale(Locale locale);
bool SetLocaleByCode(const char *code);

const LocaleInfo *GetLocaleInfo(Locale locale);
size_t GetLocaleCount();

const char *Tr(Str id);
const char *T(const char *msgid);
int Tf(char *buf, size_t buf_size, Str id, ...);

} // namespace I18n
