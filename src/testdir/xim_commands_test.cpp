#include "../xim_commands.h"
#include <cstdio>

int main()
{
    using xim::Action;
    const struct { int key; int modifiers; Action action; } cases[] = {
        {'a', 0, Action::text}, {0x754c, 0, Action::text},
        {27, 0, Action::cancel}, {3, 0, Action::copy}, {19, 0, Action::save},
        {19, XIM_SHIFT, Action::save_as}, {XIM_F4, 0, Action::save},
        {'s', XIM_CTRL, Action::save}, {'S', XIM_CTRL | XIM_SHIFT, Action::save_as},
        {XIM_LEFT, XIM_SHIFT, Action::move}, {XIM_F3, XIM_SHIFT, Action::previous},
        {XIM_BACKSPACE, 0, Action::backspace}, {XIM_IGNORE, 0, Action::ignore},
        {'a', XIM_ALT, Action::ignore}, {XIM_F10, 0, Action::menu},
        {25, 0, Action::redo}, {14, 0, Action::new_buffer}
    };
    for (const auto &test : cases)
        if (xim::resolve(test.key, test.modifiers) != test.action)
        {
            std::fprintf(stderr, "input contract failed for %d\n", test.key);
            return 1;
        }
    if (!xim::matches("Save as", "VE A") || xim::matches("Save", "unknown")) return 1;
    return 0;
}
