/*
    Copyright 2021 XITRIX

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/touch/tap_gesture.hpp>
#include <borealis/core/util.hpp>
#include <borealis/platforms/xbox360/xbox360_perf.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/hint.hpp>

using namespace brls::literals;

namespace brls
{

const std::string hintXML = R"xml(
    <brls:Box
        width="auto"
        height="auto"
        axis="row"
        paddingTop="4"
        paddingBottom="4"
        paddingLeft="16"
        paddingRight="16"
        cornerRadius="6">
            <brls:Label
                id="icon"
                width="auto"
                height="auto"
                fontSize="25.5"/>

            <brls:Label
                id="hint"
                width="auto"
                height="auto"
                fontSize="21.5"
                marginLeft="8"/>

    </brls:Box>
)xml";

Hint::Hint(std::shared_ptr<Action> action, bool allowAButtonTouch)
    : Box(Axis::ROW)
      , action(action)
{
#if defined(PLATFORM_XBOX360)
    // 第 4 轮：焦点切换 7.2s 冻结疑似卡在逐个 Hint 构造（与析构 1.45s/个对称）。
    // 拆 inflate / setText / 其余（手势+主题）三段；基类 View/Box 构造不在体内，
    // 由调用点 hint create 总时长减去本函数总时长得到残差。
    const auto hcT0 = xbox360perf::nowUsec();
#endif
    this->inflateFromXMLString(hintXML);
    this->setFocusable(false);
#if defined(PLATFORM_XBOX360)
    const auto hcT1 = xbox360perf::nowUsec();
    // 池化复用（第 5 轮）：文本/颜色统一走 applyAction；手势监听捕获 this
    // 而非 action，运行时现读 this->action —— 复用时只需换指向。
    // 守卫与原逻辑等价：(button!=A || allowTouch) && available && !inputBlocks。
    this->applyAction(action, allowAButtonTouch);
    const auto hcT2 = xbox360perf::nowUsec();
    this->addGestureRecognizer(new TapGestureRecognizer(this, [this]()
    {
        if (!this->action || !this->action->isAvailable() || Application::isInputBlocks())
            return;
        if (this->action->getButton() == BUTTON_A && !this->allowAButtonTouch)
            return;
        this->action->getActionListener()(this);
    }));
#else
    icon->setText(getKeyIcon(static_cast<ControllerButton>(action->getButton())));
    hint->setText(action->getHintText());

    if ((action->getButton() != BUTTON_A || allowAButtonTouch) && action->isAvailable() && !Application::isInputBlocks())
    {
        this->addGestureRecognizer(new TapGestureRecognizer(this, [this, action]()
        {
            action->getActionListener()(this);
        }));
    }

    if (!action->isAvailable() || Application::isInputBlocks())
    {
        Theme theme = Application::getTheme();
        icon->setTextColor(theme["brls/text_disabled"]);
        hint->setTextColor(theme["brls/text_disabled"]);
    }
#endif
#if defined(PLATFORM_XBOX360)
    const double hcTotal = xbox360perf::msSince(hcT0);
    if (hcTotal > 200)
        X360_PROBE_INFO("[x360-perf] Hint ctor {:.0f} ms | inflate {:.0f} text {:.0f} rest {:.0f}",
                     hcTotal, (hcT1 - hcT0) / 1000.0, (hcT2 - hcT1) / 1000.0,
                     (xbox360perf::nowUsec() - hcT2) / 1000.0);
#endif
}

#if defined(PLATFORM_XBOX360)
void Hint::applyAction(const std::shared_ptr<Action>& action, bool allowAButtonTouch)
{
    this->action            = action;
    this->allowAButtonTouch = allowAButtonTouch;

    icon->setText(getKeyIcon(static_cast<ControllerButton>(action->getButton())));
    hint->setText(action->getHintText());

    Theme theme = Application::getTheme();
    if (!action->isAvailable() || Application::isInputBlocks())
    {
        icon->setTextColor(theme["brls/text_disabled"]);
        hint->setTextColor(theme["brls/text_disabled"]);
    }
    else
    {
        // 复用时从禁用态恢复：Label 构造默认色即 theme["brls/text"]
        //（label.cpp:217），与原 ctor 只在禁用时覆写的效果一致。
        icon->setTextColor(theme["brls/text"]);
        hint->setTextColor(theme["brls/text"]);
    }
}
#endif

std::string Hint::getKeyIcon(ControllerButton button, bool ignoreKeysSwap)
{
    if (!ignoreKeysSwap)
        button = InputManager::mapControllerState(button);

    switch (button)
    {
        case BUTTON_A:
            return "\uE0E0";
        case BUTTON_B:
            return "\uE0E1";
        case BUTTON_X:
            return "\uE0E2";
        case BUTTON_Y:
            return "\uE0E3";
        case BUTTON_LSB:
            return "\uE104";
        case BUTTON_RSB:
            return "\uE105";
        case BUTTON_LT:
            return "\uE0E6";
        case BUTTON_RT:
            return "\uE0E7";
        case BUTTON_LB:
            return "\uE0E4";
        case BUTTON_RB:
            return "\uE0E5";
        case BUTTON_START:
            return "\uE0EF";
        case BUTTON_BACK:
            return "\uE0F0";
        case BUTTON_LEFT:
            return "\uE0ED";
        case BUTTON_UP:
            return "\uE0EB";
        case BUTTON_RIGHT:
            return "\uE0EE";
        case BUTTON_DOWN:
            return "\uE0EC";
        default:
            return "\uE152";
    }
}

Hints::Hints()
{
    setAxis(Axis::ROW);
    setDirection(Direction::LEFT_TO_RIGHT);

    hintSubscription = Application::getGlobalHintsUpdateEvent()->subscribe([this]()
    {
        if (!AppletFrame::HIDE_BOTTOM_BAR || forceShown)
        {
            refillHints(Application::getCurrentFocus());
        }
    });

    this->registerBoolXMLAttribute("addBaseAction", [this](bool value)
    {
        this->setAddUnableAButtonAction(value);
    });

    this->registerBoolXMLAttribute("allowAButtonTouch", [this](bool value)
    {
        this->setAllowAButtonTouch(value);
    });

    this->registerBoolXMLAttribute("forceShown", [this](bool value)
    {
        this->forceShown = value;
    });
}

Hints::~Hints()
{
    Application::getGlobalHintsUpdateEvent()->unsubscribe(hintSubscription);
#if defined(PLATFORM_XBOX360)
    // 池化的 Hint 全部手动释放：当前 children 必然都来自池（refill 只挂池成员），
    // 先摘下避免 ~Box 重复删除；不在池里的（理论上没有）留给 ~Box 兜底。
    const std::vector<View*> kids = this->getChildren();
    for (View* v : kids)
    {
        const bool pooled = std::any_of(hintPool.begin(), hintPool.end(),
                                        [v](const std::pair<const ControllerButton, Hint*>& kv)
                                        { return kv.second == v; });
        if (pooled)
            this->removeView(v, false);
    }
    for (auto& kv : hintPool)
        delete kv.second;
    hintPool.clear();
#endif
}

static int buttonToSortableVal(ControllerButton button)
{
    // From left to right:
    //  - first +
    //  - then all hints that are not B and A in original order
    //  - finally B and A

    switch (button)
    {
        case BUTTON_START:
            return 0;
        case BUTTON_B:
            return 2;
        case BUTTON_A:
            return 3;
        default:
            return 1;
    }
}

static bool actionsSortFunc(const std::shared_ptr<Action>& a, const std::shared_ptr<Action>& b)
{
    return buttonToSortableVal(static_cast<ControllerButton>(a->getButton())) < buttonToSortableVal(static_cast<ControllerButton>(b->getButton()));
}

void Hints::refillHints(View* focusView)
{
    if (!focusView)
        return;

    // 每次焦点切换都会走到这里（第 3 轮）：三段各自可被看门狗点名，
    // 与 giveFocus 的 focus:hints 段衔接。非 Xbox 是空内联。
    xbox360perf::stageMarkIfMain("hints:refill");

    // 第 5 轮根因：每次焦点切换都 clearViews（= N×Hint 慢析构，实测 0.6~1.3s/个）
    // + 全量 new Hint（inflate = 2~3 个 Label View ctor，实测 0.5~1.4s/个）。
    // Xbox 改为池化复用：旧 Hint 摘下不销毁，按键命中池就只 applyAction
    //（两次 setText + 主题查找，µs 级），缺的按键才新建（每个按钮一生最多一次）。
    // 非 Xbox 保持原全量重建路径，行为逐字节不变。
    xbox360perf::stageMarkIfMain("hints:collect");

    std::set<ControllerButton> addedButtons; // we only ever want one action per key
    std::vector<std::shared_ptr<Action> > actions;

    while (focusView != nullptr)
    {
        for (auto& action : focusView->getActions())
        {
            if (action->getType() != ACTION_GAMEPAD)
                continue; // only show gamepad actions in hints

            if (action->isHidden())
                continue;

            if (addedButtons.find(static_cast<ControllerButton>(action->getButton())) != addedButtons.end())
                continue;

            if (Application::isHintsLiteMode() && action->getButton() != BUTTON_A && action->getButton() != BUTTON_B)
            {
                continue;
            }

            addedButtons.insert(static_cast<ControllerButton>(action->getButton()));
            actions.push_back(action);
        }

        focusView = focusView->getParent();
    }

    const auto it = std::find_if(actions.begin(), actions.end(), [](const std::shared_ptr<Action>& action)
    {
        return *action == BUTTON_A;
    });

    if (addUnableAButtonAction && it == actions.end())
    {
        actions.push_back(std::make_shared<GamepadAction>(BUTTON_A, 0, "hints/ok"_i18n, false, false, false, Sound::SOUND_NONE, nullptr));
    }

    // Sort the actions
    std::stable_sort(actions.begin(), actions.end(), actionsSortFunc);

#if defined(PLATFORM_XBOX360)
    xbox360perf::stageMarkIfMain("hints:attach");
    // 摘下当前所有子视图（free=false：不进删除池，慢析构源头就此消除），
    // 再按排序顺序重新挂载 —— 行序与原 addView 顺序一致。
    const std::vector<View*> previousChildren = this->getChildren();
    for (View* child : previousChildren)
        this->removeView(child, false);

    int hintIdx = 0;
    for (auto& action : actions)
    {
        const auto button  = static_cast<ControllerButton>(action->getButton());
        auto it            = hintPool.find(button);
        Hint* hint         = (it != hintPool.end()) ? it->second : nullptr;
        const bool created = (hint == nullptr);
        if (created)
        {
            hint = new Hint(action, allowAButtonTouch);
            hintPool[button] = hint;
        }
        else
        {
            const auto hnT0 = xbox360perf::nowUsec();
            hint->applyAction(action, allowAButtonTouch);
            const double hnMs = xbox360perf::msSince(hnT0);
            if (hnMs > 200)
                X360_PROBE_INFO("[x360-perf] hint reuse took {:.0f} ms ({}/{})",
                             hnMs, hintIdx + 1, (int)actions.size());
        }
        this->addView(hint);
        hintIdx++;
        if (created)
        {
            // 新建路径的耗时由 Hint ctor 内部的 >200ms 分段日志负责
            //（inflate/text/rest），这里只补一行定位锚点。
            X360_PROBE_DEBUG("[x360-perf] hint created for button {} ({}/{})",
                          (int)button, hintIdx, (int)actions.size());
        }
    }
#else
    xbox360perf::stageMarkIfMain("hints:clear");
    clearViews();

    for (auto action : actions)
    {
        Hint* hint = new Hint(action, allowAButtonTouch);
        addView(hint);
    }
#endif
}

View* Hints::create()
{
    return new Hints();
}

} // namespace brls