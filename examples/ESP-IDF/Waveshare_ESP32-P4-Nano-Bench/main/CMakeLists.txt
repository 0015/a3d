idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    # esp_lcd_touch is needed even though this file never calls it directly:
    # board.h declares board_touch_init() in terms of esp_lcd_touch_handle_t,
    # so the header is on the include path of anything that includes board.h.
    # The board component keeps it PRIVATE, which is right for the component
    # and means every consumer has to ask for it - the sibling STL example's
    # main does exactly the same.
    REQUIRES a3d_demo p4_nano_board esp_lcd esp_lcd_touch esp_timer heap freertos
)
