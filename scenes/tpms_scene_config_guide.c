#include "../tpms_app_i.h"

void tpms_scene_config_guide_on_enter(void* context) {
    TPMSApp* app = context;
    Widget* widget = app->widget;

    widget_add_text_box_element(
        widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#Configuration Guide\e!\n",
        false);

    widget_add_text_scroll_element(
        widget,
        0,
        16,
        128,
        48,
        "\e#VEHICLE SETTINGS:\e!\n"
        "\n"
        "\e#Ford/Lincoln/Mercury:\e!\n"
        "315MHz + FM476\n"
        "\n"
        "\e#GM/Chevrolet/Cadillac:\e!\n"
        "315MHz + FM238\n"
        "\n"
        "\e#Toyota/Lexus/Scion:\e!\n"
        "315MHz + FM476\n"
        "\n"
        "\e#Nissan/Infiniti:\e!\n"
        "433MHz + AM650\n"
        "\n"
        "\e#Hyundai/Kia/Genesis:\e!\n"
        "433MHz + FM238\n"
        "\n"
        "\e#Schrader (Aftermarket):\e!\n"
        "433MHz + AM650\n"
        "\n"
        "\e#OPERATING MODES:\e!\n"
        "\n"
        "\e#Fixed Mode (Recommended):\e!\n"
        "• Set your vehicle's exact freq/mod\n"
        "• Turn hopping OFF\n"
        "• 100% signal reliability\n"
        "• Best for daily monitoring\n"
        "\n"
        "\e#Hopping Mode:\e!\n"
        "• Scans all frequencies\n"
        "• Turn hopping ON\n"
        "• Misses ~80% of transmissions\n"
        "• Good for discovery/testing\n"
        "\n"
        "\e#WHY SIGNALS GET MISSED:\e!\n"
        "• TPMS bursts: 10-50ms\n"
        "• Hopping cycle: 100ms per freq\n"
        "• Only listening 20% of time\n"
        "• Sensors transmit randomly\n"
        "\n"
        "\e#QUICK SETUP:\e!\n"
        "1. Know your car? Use Fixed mode\n"
        "2. Unknown car? Start with Hopping\n"
        "3. Found signals? Switch to Fixed\n"
        "4. Multiple cars? Stay on Hopping"
    );

    view_dispatcher_switch_to_view(app->view_dispatcher, TPMSViewWidget);
}

bool tpms_scene_config_guide_on_event(void* context, SceneManagerEvent event) {
    TPMSApp* app = context;
    bool consumed = false;
    UNUSED(app);

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void tpms_scene_config_guide_on_exit(void* context) {
    TPMSApp* app = context;
    widget_reset(app->widget);
}
