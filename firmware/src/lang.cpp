#include <Preferences.h>
#include "lang.h"

// ---- Animated status verbs ------------------------------------------------

static const char* const WORDS_EN[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};

static const char* const WORDS_RU[] = {
    "Вершит", "Проясняет", "Штудирует",
    "Действует", "Чарует", "Философствует",
    "Воплощает", "Прозревает", "Раздумывает",
    "Печёт", "Хитрит", "Вещает",
    "Бупает", "Балаболит", "Обрабатывает",
    "Варит", "Куёт", "Возится",
    "Считает", "Формирует", "Ломает голову",
    "Мозгует", "Резвится", "Ретикулирует",
    "Транслирует", "Генерирует", "Пережёвывает",
    "Взбивает", "Проращивает", "Замышляет",
    "Клодит", "Высиживает", "Тащит",
    "Сплавляет", "Пасёт", "Пританцовывает",
    "Кумекает", "Бибикает", "Лущит",
    "Комбобулирует", "Шустрит", "Томит",
    "Вычисляет", "Придумывает", "Мнёт",
    "Стряпает", "Воображает", "Копает вглубь",
    "Колдует", "Инкубирует", "Крутит",
    "Взвешивает", "Умозаключает", "Тушит",
    "Созерцает", "Джайвит", "Просекает",
    "Готовит", "Манифестирует", "Синтезирует",
    "Мастерит", "Маринует", "Думает",
    "Творит", "Петляет", "Ковыряется",
    "Хрустит", "Бредёт", "Трансмутирует",
    "Расшифровывает", "Обмозговывает", "Разворачивает",
    "Совещается", "Собирается", "Распутывает",
    "Определяет", "Грезит", "Вайбит",
    "Дискомбобулирует", "Наигрывает", "Блуждает",
    "Гадает", "Процеживает", "Жужжит",
    "Делает", "Колышется",
    "Осуществляет", "Волшебничает",
    "Работает", "Укрощает",
};

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

// ---- Tables ---------------------------------------------------------------

static const Strings STR_EN = {
    /* title            */ "Usage",
    /* pill_session     */ "Session",
    /* pill_weekly      */ "Weekly",
    /* pill_period      */ "Period",
    /* spending         */ "Spending",
    /* of_monthly_budget*/ "of monthly budget",
    /* pace_under/on/over */ "Under pace", "On pace", "Over pace",
    /* reset_m          */ "Resets in %dm%s",
    /* reset_hm         */ "Resets in %dh %dm%s",
    /* reset_dh         */ "Resets in %dd %dh%s",
    /* ent_reset_fmt    */ "#%s %s# - #%s Resets %s#",
    /* pair1/2/3        */ "To pair", "hold the power button", "for 3 seconds, then release",
    /* st_waiting       */ "Waiting",
    /* st_no_data       */ "No data",
    /* st_listening     */ "Listening",
    /* st_connected     */ "Connected",
    /* st_resting       */ "Resting",
    /* err auth/token/rate/net/api */
    "Update token", "No token", "Rate limited", "No network", "API error",
    /* attn_caption     */ {
        "Awaiting your reply",   // ATTN_INPUT
        "Permission needed",     // ATTN_PERM
        "Done!",                 // ATTN_DONE
        "Meeting soon!",         // ATTN_CAL
        "Meeting started!",      // ATTN_CAL_START
        "Limit almost hit!",     // ATTN_LIMIT
        "Limits refreshed!",     // ATTN_RESET
    },
    /* attn_status      */ {
        "Awaiting reply", "Awaiting approval", "Done", "Meeting soon",
        "In a meeting", "Limit near", "Limits refreshed",
    },
    WORDS_EN, COUNT(WORDS_EN),
};

static const Strings STR_RU = {
    /* title            */ "Лимиты",
    /* pill_session     */ "Сессия",
    /* pill_weekly      */ "Неделя",
    /* pill_period      */ "Период",
    /* spending         */ "Расходы",
    /* of_monthly_budget*/ "месячного бюджета",
    /* pace_under/on/over */ "Ниже темпа", "В темпе", "Выше темпа",
    /* reset_m          */ "Сброс через %dм%s",
    /* reset_hm         */ "Сброс через %dч %dм%s",
    /* reset_dh         */ "Сброс через %dд %dч%s",
    /* ent_reset_fmt    */ "#%s %s# - #%s Сброс %s#",
    /* pair1/2/3        */ "Сопряжение", "зажмите кнопку питания", "на 3 секунды и отпустите",
    /* st_waiting       */ "Ожидание",
    /* st_no_data       */ "Нет данных",
    /* st_listening     */ "Слушает",
    /* st_connected     */ "Подключено",
    /* st_resting       */ "Отдыхает",
    /* err auth/token/rate/net/api */
    "Обновите токен", "Нет токена", "Лимит запросов", "Нет сети", "Ошибка API",
    /* attn_caption     */ {
        "Клод ждёт ответа",      // ATTN_INPUT
        "Нужно разрешение",      // ATTN_PERM
        "Готово!",               // ATTN_DONE
        "Скоро встреча!",        // ATTN_CAL
        "Встреча началась!",     // ATTN_CAL_START
        "Лимит близко!",         // ATTN_LIMIT
        "Лимиты обновились!",    // ATTN_RESET
    },
    /* attn_status      */ {
        "Ждёт ответа", "Ждёт разрешения", "Готово", "Скоро встреча",
        "Встреча идёт", "Лимит близко", "Лимиты обновились",
    },
    WORDS_RU, COUNT(WORDS_RU),
};

// ---- Selection + persistence ----------------------------------------------

// One lookup used by both init and set — adding a language is one row here
// plus its STR_XX table.
static const struct { const char* code; const Strings* table; } LANGS[] = {
    { "en", &STR_EN },
    { "ru", &STR_RU },
};

static const Strings* lang_lookup(const char* code) {
    for (auto& l : LANGS)
        if (!strcmp(code, l.code)) return l.table;
    return nullptr;
}

const Strings* S = &STR_EN;

void strings_init(void) {
    // Same open/get/close idiom as brightness.cpp — no held-open handle.
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    String lang = prefs.getString("lang", "");
    prefs.end();
    const Strings* t = lang_lookup(lang.c_str());
    if (t) S = t;
}

bool strings_set_lang(const char* lang) {
    const Strings* want = lang_lookup(lang);
    if (!want) return false;         // absent/unknown — keep the current table
    if (want == S) return false;
    S = want;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putString("lang", lang);
    prefs.end();
    return true;
}
