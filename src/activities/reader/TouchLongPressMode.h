#pragma once

// Mode passed from the reader to DictionaryWordSelectActivity when a touch
// long-press opens word-select, so the activity never reads the mutable
// SETTINGS.touchLongPressAction global. Defined here (not in either activity
// header) so both can include it without a circular dependency.
enum class TouchLongPressMode { Dictionary, Footnote };
