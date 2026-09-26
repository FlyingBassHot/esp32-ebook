// 天气页布局 UI
//
// 负责创建天气页的所有 LVGL 控件：
// - 状态栏（右上角白底胶囊：WiFi + 电池）
// - 顶栏中央「小智」+ 状态图标
// - 左上角温湿度标签
// - 时钟卡片（左上）
// - 合并卡片（右上：星期/日期 + 地点/天气 + 备忘录居中 2 行轮播）
// - 下排：照片轮播（自适应宽）+ 书目列表（纯展示轮播）
// - 基类占位控件（防止空指针崩溃）
// - 轮播定时器（备忘录 4s / 照片 10s / 书目 6s）

#include "custom_lcd_display.h"
#include <esp_log.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>

// 声明天气站专用字体（从 MyWeatherStation 移植，字符集有限但够天气站用）
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 声明小智自带字体（7415 个常用汉字）
LV_FONT_DECLARE(font_puhui_16_4);  // 16px 标准字体
LV_FONT_DECLARE(font_puhui_14_1);  // 14px 小字体

// 声明状态栏图标（从 MyWeatherStation 移植）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "WeatherUI";

// ===== 内部工具：轮播窗口渲染（调用者需已持有 DisplayLock）=====

void CustomLcdDisplay::RenderMemoWindow() {
    if (!memo_list_label_) return;
    if (memo_lines_.empty()) {
        lv_label_set_text(memo_list_label_, "暂无待办");
        return;
    }
    int n = static_cast<int>(memo_lines_.size());
    if (memo_view_idx_ >= n) memo_view_idx_ = 0;
    std::string text;
    int rows = (n < 2) ? n : 2;
    for (int i = 0; i < rows; i++) {
        int idx = (memo_view_idx_ + i) % n;
        if (i) text += "\n";
        text += memo_lines_[idx];
    }
    lv_label_set_text(memo_list_label_, text.c_str());
}

void CustomLcdDisplay::ScanPhotos() {
    photo_files_.clear();
    DIR *dir = opendir("/sdcard/photos");
    if (!dir) {
        ESP_LOGI(TAG, "无照片目录 /sdcard/photos");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() > 4) {
            std::string ext = name.substr(name.size() - 4);
            for (auto &c : ext) c = static_cast<char>(std::tolower(c));
            if (ext == ".png") photo_files_.push_back("/sdcard/photos/" + name);
        }
    }
    closedir(dir);
    std::sort(photo_files_.begin(), photo_files_.end());
    ESP_LOGI(TAG, "扫描到 %d 张照片", static_cast<int>(photo_files_.size()));
}

void CustomLcdDisplay::LayoutBottomRow() {
    const int bot_y = 170;
    const int bot_h = 122;
    const int pad = 8;
    const int gap = 6;

    int x = pad;
    if (photo_files_.empty() || photo_card_w_ <= 0) {
        if (photo_card_) lv_obj_add_flag(photo_card_, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (photo_card_) {
            lv_obj_remove_flag(photo_card_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(photo_card_, x, bot_y);
            lv_obj_set_size(photo_card_, photo_card_w_, bot_h);
        }
        x += photo_card_w_ + gap;
    }

    int w = 400 - pad - x;
    if (book_card_) {
        lv_obj_set_pos(book_card_, x, bot_y);
        lv_obj_set_size(book_card_, w, bot_h);
    }
    if (book_list_label_) lv_obj_set_width(book_list_label_, w - 16);
    if (book_sep_) lv_obj_set_width(book_sep_, w - 16);
}

void CustomLcdDisplay::ShowNextPhoto() {
    DisplayLockGuard lock(this);
    if (photo_files_.empty()) {
        photo_card_w_ = 0;
        LayoutBottomRow();
        return;
    }

    // 循环推进，坏图剔除后重试
    for (int attempt = 0; attempt < 3 && !photo_files_.empty(); attempt++) {
        photo_index_ = (photo_index_ + 1) % static_cast<int>(photo_files_.size());
        std::string path = "S:" + photo_files_[photo_index_];
        lv_image_header_t hdr;
        if (lv_image_decoder_get_info(path.c_str(), &hdr) == LV_RESULT_OK) {
            int w = hdr.w;
            if (w > 168) w = 168;
            if (w < 60) w = 60;
            photo_card_w_ = w;
            lv_image_set_src(photo_img_, path.c_str());
            LayoutBottomRow();
            return;
        }
        ESP_LOGW(TAG, "无法解码照片: %s", photo_files_[photo_index_].c_str());
        photo_files_.erase(photo_files_.begin() + photo_index_);
        photo_index_ = -1;
    }
    photo_card_w_ = 0;
    LayoutBottomRow();
}

void CustomLcdDisplay::RefreshBookList() {
    DisplayLockGuard lock(this);
    book_lines_ = ReaderListBooks();
    book_view_idx_ = 0;
    RenderBookWindow();
}

void CustomLcdDisplay::RenderBookWindow() {
    if (!book_list_label_) return;
    int n = static_cast<int>(book_lines_.size());
    if (n == 0) {
        lv_label_set_text(book_list_label_, "暂无书籍");
        return;
    }
    std::string text;
    if (n <= 4) {
        for (int i = 0; i < n; i++) {
            if (i) text += "\n";
            text += book_lines_[i];
        }
    } else {
        if (book_view_idx_ >= n) book_view_idx_ = 0;
        for (int i = 0; i < 4; i++) {
            if (i) text += "\n";
            text += book_lines_[(book_view_idx_ + i) % n];
        }
    }
    lv_label_set_text(book_list_label_, text.c_str());
}

// ===== 轮播定时器回调 =====

void CustomLcdDisplay::MemoTimerCb(lv_timer_t *t) {
    auto *self = (CustomLcdDisplay *)lv_timer_get_user_data(t);
    DisplayLockGuard lock(self);
    if (self->memo_lines_.size() < 2) return;
    self->memo_view_idx_ = (self->memo_view_idx_ + 1) % static_cast<int>(self->memo_lines_.size());
    self->RenderMemoWindow();
}

void CustomLcdDisplay::PhotoTimerCb(lv_timer_t *t) {
    auto *self = (CustomLcdDisplay *)lv_timer_get_user_data(t);
    if (self->photo_files_.empty()) return;
    self->ShowNextPhoto();
}

void CustomLcdDisplay::BookTimerCb(lv_timer_t *t) {
    auto *self = (CustomLcdDisplay *)lv_timer_get_user_data(t);
    DisplayLockGuard lock(self);
    int n = static_cast<int>(self->book_lines_.size());
    if (n <= 4) return;
    self->book_view_idx_ = (self->book_view_idx_ + 4) % n;
    self->RenderBookWindow();
}

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);
    
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    weather_page_ = lv_obj_create(root);
    lv_obj_set_size(weather_page_, 400, 300);
    lv_obj_set_pos(weather_page_, 0, 0);
    lv_obj_set_style_bg_opa(weather_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_page_, 0, 0);
    lv_obj_set_style_pad_all(weather_page_, 0, 0);
    lv_obj_set_style_radius(weather_page_, 0, 0);
    lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *screen = weather_page_;

    const lv_font_t *font_small  = &alibaba_puhui_16;
    const lv_font_t *font_normal = &alibaba_puhui_24;
    const lv_font_t *font_clock  = &alibaba_black_64;
    const lv_font_t *font_tiny   = &font_puhui_14_1;

    // ===== 状态栏（右上角白底胶囊）=====
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, 115, 28);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 14, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 5, 0);

    // WiFi 图标（我们自己的图片图标，不给基类用）
    wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(wifi_icon_img_, &ui_img_wifi_off);

    // 电池图标
    battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(battery_icon_img_, &ui_img_battery_full);

    // 电量百分比文字
    battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(battery_pct_label_, font_small, 0);
    lv_obj_set_style_text_color(battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(battery_pct_label_, "---%");

    // ===== 顶栏中央「小智」+ 状态图标 =====
    CreateTopStatus(screen);

    // ===== 左上角温湿度（白字，直接在黑底上）=====
    sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sensor_label_, font_small, 0);
    lv_obj_set_style_text_color(sensor_label_, lv_color_white(), 0);
    lv_obj_align(sensor_label_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_label_set_text(sensor_label_, "--.-°C  --.-%");

    // ===== 上排两卡 =====
    const int pad = 8;
    const int gap = 6;
    const int top_y = 36;
    const int top_row_h = 128;
    const int left_w = 248;
    const int right_w = 400 - pad * 2 - left_w - gap;  // = 130

    // --- 左上：时钟卡片 ---
    lv_obj_t *time_card = lv_obj_create(screen);
    lv_obj_set_pos(time_card, pad, top_y);
    lv_obj_set_size(time_card, left_w, top_row_h);
    lv_obj_set_style_border_width(time_card, 2, 0);
    lv_obj_set_style_border_color(time_card, lv_color_black(), 0);
    lv_obj_set_style_radius(time_card, 15, 0);
    lv_obj_set_style_bg_color(time_card, lv_color_white(), 0);
    lv_obj_set_style_pad_all(time_card, 0, 0);
    lv_obj_remove_flag(time_card, LV_OBJ_FLAG_SCROLLABLE);

    time_label_ = lv_label_create(time_card);
    lv_obj_set_style_text_color(time_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(time_label_, font_clock, 0);
    lv_obj_set_style_text_letter_space(time_label_, 2, 0);
    lv_obj_center(time_label_);
    lv_label_set_text(time_label_, "00:00");

    // 时钟卡片内边框装饰
    lv_obj_t *time_inner = lv_obj_create(time_card);
    lv_obj_set_size(time_inner, left_w - 14, top_row_h - 14);
    lv_obj_center(time_inner);
    lv_obj_set_style_bg_opa(time_inner, 0, 0);
    lv_obj_set_style_border_width(time_inner, 2, 0);
    lv_obj_set_style_border_color(time_inner, lv_color_black(), 0);
    lv_obj_set_style_radius(time_inner, 10, 0);
    lv_obj_remove_flag(time_inner, LV_OBJ_FLAG_SCROLLABLE);

    // --- 右上：合并卡片（星期/日期 | 地点/天气 | 备忘录 2 行）---
    int right_x = pad + left_w + gap;

    lv_obj_t *calendar_card = lv_obj_create(screen);
    lv_obj_set_pos(calendar_card, right_x, top_y);
    lv_obj_set_size(calendar_card, right_w, top_row_h);
    lv_obj_set_style_border_width(calendar_card, 3, 0);
    lv_obj_set_style_border_color(calendar_card, lv_color_white(), 0);
    lv_obj_set_style_radius(calendar_card, 15, 0);
    lv_obj_set_style_bg_color(calendar_card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(calendar_card, 0, 0);
    lv_obj_remove_flag(calendar_card, LV_OBJ_FLAG_SCROLLABLE);

    // 左列：星期（16px）+ 日期（24px）
    day_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(day_label_, font_small, 0);
    lv_obj_set_style_text_color(day_label_, lv_color_white(), 0);
    lv_obj_align(day_label_, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_label_set_text(day_label_, "---");

    date_num_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(date_num_label_, font_normal, 0);
    lv_obj_set_style_text_color(date_num_label_, lv_color_white(), 0);
    lv_obj_align(date_num_label_, LV_ALIGN_TOP_LEFT, 8, 26);
    lv_label_set_text(date_num_label_, "--");

    // 右列：地点（上）+ 天气（下），均 14px
    city_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(city_label_, font_tiny, 0);
    lv_obj_set_style_text_color(city_label_, lv_color_white(), 0);
    lv_obj_set_pos(city_label_, 64, 4);
    lv_obj_set_width(city_label_, 54);
    lv_label_set_long_mode(city_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(city_label_, "--");

    weather_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(weather_label_, font_tiny, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_white(), 0);
    lv_obj_set_pos(weather_label_, 64, 24);
    lv_obj_set_width(weather_label_, 54);
    lv_label_set_long_mode(weather_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(weather_label_, "-- --°C");

    // 分隔线
    lv_obj_t *cal_sep = lv_obj_create(calendar_card);
    lv_obj_set_size(cal_sep, 108, 1);
    lv_obj_set_style_bg_color(cal_sep, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cal_sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cal_sep, 0, 0);
    lv_obj_set_pos(cal_sep, 8, 64);
    lv_obj_remove_flag(cal_sep, LV_OBJ_FLAG_SCROLLABLE);

    // 备忘录：居中 2 行轮播
    memo_list_label_ = lv_label_create(calendar_card);
    lv_obj_set_style_text_font(memo_list_label_, font_tiny, 0);
    lv_obj_set_style_text_color(memo_list_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(memo_list_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(memo_list_label_, 116);
    lv_obj_set_height(memo_list_label_, 46);
    lv_label_set_long_mode(memo_list_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(memo_list_label_, 4, 70);
    lv_label_set_text(memo_list_label_, "暂无待办");

    // ===== 下排：照片轮播 + 书目列表 =====
    // 照片卡（白底黑框，图片撑满高度 122、宽随比例，居中裁剪超出部分）
    photo_card_ = lv_obj_create(screen);
    lv_obj_set_pos(photo_card_, pad, 170);
    lv_obj_set_size(photo_card_, 160, 122);
    lv_obj_set_style_border_width(photo_card_, 2, 0);
    lv_obj_set_style_border_color(photo_card_, lv_color_black(), 0);
    lv_obj_set_style_radius(photo_card_, 12, 0);
    lv_obj_set_style_bg_color(photo_card_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(photo_card_, 0, 0);
    lv_obj_remove_flag(photo_card_, LV_OBJ_FLAG_SCROLLABLE);

    photo_img_ = lv_image_create(photo_card_);
    lv_obj_center(photo_img_);
    lv_obj_add_flag(photo_card_, LV_OBJ_FLAG_HIDDEN);  // 扫描到照片后再显示

    // 书目卡（黑底白框，标题 + 分隔线 + 居中书目 4 行）
    book_card_ = lv_obj_create(screen);
    lv_obj_set_pos(book_card_, pad + 160 + gap, 170);
    lv_obj_set_size(book_card_, 400 - pad * 2 - gap - 160, 122);
    lv_obj_set_style_border_width(book_card_, 2, 0);
    lv_obj_set_style_border_color(book_card_, lv_color_white(), 0);
    lv_obj_set_style_radius(book_card_, 12, 0);
    lv_obj_set_style_bg_color(book_card_, lv_color_black(), 0);
    lv_obj_set_style_pad_all(book_card_, 0, 0);
    lv_obj_remove_flag(book_card_, LV_OBJ_FLAG_SCROLLABLE);

    book_title_label_ = lv_label_create(book_card_);
    lv_obj_set_style_text_font(book_title_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(book_title_label_, lv_color_white(), 0);
    lv_obj_set_pos(book_title_label_, 8, 3);
    lv_label_set_text(book_title_label_, "书目");

    book_sep_ = lv_obj_create(book_card_);
    lv_obj_set_size(book_sep_, 160, 1);
    lv_obj_set_pos(book_sep_, 8, 22);
    lv_obj_set_style_bg_color(book_sep_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(book_sep_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(book_sep_, 0, 0);
    lv_obj_remove_flag(book_sep_, LV_OBJ_FLAG_SCROLLABLE);

    book_list_label_ = lv_label_create(book_card_);
    lv_obj_set_style_text_font(book_list_label_, font_tiny, 0);
    lv_obj_set_style_text_color(book_list_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(book_list_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(book_list_label_, 160);
    lv_obj_set_height(book_list_label_, 78);
    lv_obj_set_style_text_line_space(book_list_label_, 4, 0);
    lv_label_set_long_mode(book_list_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(book_list_label_, 8, 28);
    lv_label_set_text(book_list_label_, "暂无书籍");

    // 首张照片 + 书目扫描（SD 卡此时已挂载）
    ScanPhotos();
    ShowNextPhoto();
    RefreshBookList();

    // ===== 轮播定时器 =====
    memo_timer_ = lv_timer_create(MemoTimerCb, 4000, this);
    photo_timer_ = lv_timer_create(PhotoTimerCb, 10000, this);
    book_timer_ = lv_timer_create(BookTimerCb, 6000, this);
    status_anim_timer_ = lv_timer_create(StatusAnimTimerCb, 300, this);
    lv_timer_pause(status_anim_timer_);

    // ===== 基类占位控件（防止基类方法空指针崩溃）=====
    // container_ 是 SetTheme 必须操作的（设置背景图/颜色），
    // 创建一个隐藏的 1×1 容器
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(screen);
    lv_label_set_text(network_label_, "");
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
    
    battery_label_ = lv_label_create(screen);
    lv_label_set_text(battery_label_, "");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    
    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    
    mute_label_ = lv_label_create(screen);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);
    
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(low_battery_label_, 300);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");
    
    // emoji 相关占位（SetEmotion 已重写为空实现，保留占位防空指针）
    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    
    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // chat_message_label_ 指向隐藏占位标签（基类析构时会释放它）
    chat_message_label_ = lv_label_create(screen);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "天气页 UI 创建完成");
}
