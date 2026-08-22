#include "fonts.h"

LV_FONT_DECLARE(font_agent_small_bold_18);
LV_FONT_DECLARE(font_agent_medium_bold_28);
LV_FONT_DECLARE(font_agent_large_bold_56);
#if LV_FONT_MONTSERRAT_18
LV_FONT_DECLARE(lv_font_montserrat_18);
#endif
LV_FONT_DECLARE(font_agent_home_name_bold_35);
LV_FONT_DECLARE(font_agent_home_number_bold_70);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace agent_ui::fonts {

const lv_font_t* LargeBold() { return &font_agent_large_bold_56; }
const lv_font_t* MediumBold() { return &font_agent_medium_bold_28; }
const lv_font_t* SmallBold() { return &font_agent_small_bold_18; }
const lv_font_t* Large() { return LargeBold(); }
const lv_font_t* Medium() { return MediumBold(); }
const lv_font_t* Small() { return SmallBold(); }
const lv_font_t* Keyboard() {
#if LV_FONT_MONTSERRAT_18
    return &lv_font_montserrat_18;
#else
    return Small();
#endif
}
const lv_font_t* HomeNameBold() { return &font_agent_home_name_bold_35; }
const lv_font_t* HomeNumberBold() { return &font_agent_home_number_bold_70; }
const lv_font_t* Icon() { return &font_awesome_20_4; }
const lv_font_t* IconLarge() { return &font_awesome_30_4; }

}  // namespace agent_ui::fonts
