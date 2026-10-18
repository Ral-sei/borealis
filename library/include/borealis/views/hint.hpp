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

#pragma once

#include <borealis/core/application.hpp>
#include <borealis/core/bind.hpp>
#include <borealis/core/box.hpp>
#include <borealis/views/image.hpp>
#include <borealis/views/label.hpp>

#include <map>

namespace brls
{

class Hint : public Box
{
  public:
    Hint(std::shared_ptr<Action> action, bool allowAButtonTouch = false);
    static std::string getKeyIcon(ControllerButton button, bool ignoreKeysSwap = false);
#if defined(PLATFORM_XBOX360)
    // 第 5 轮：焦点切换冻结已归因到“每次 refill 全量 new/delete Hint”。
    // 池化复用时调用：只更新动作指向、按钮图标、提示文本与禁用色，
    // 不再走 inflate（~700ms）。手势监听里现读 this->action，换指向即生效。
    void applyAction(const std::shared_ptr<Action>& action, bool allowAButtonTouch);
#endif

  private:
    std::shared_ptr<Action> action;
#if defined(PLATFORM_XBOX360)
    bool allowAButtonTouch = false;
#endif

    BRLS_BIND(Label, icon, "icon");
    BRLS_BIND(Label, hint, "hint");
};

class Hints : public Box
{
  public:
    Hints();
    ~Hints();

    void setAddUnableAButtonAction(bool value)
    {
        addUnableAButtonAction = value;
    }

    bool getAddUnableAButtonAction() const
    {
        return addUnableAButtonAction;
    }

    void setAllowAButtonTouch(bool value)
    {
        allowAButtonTouch = value;
    }

    bool getAllowAButtonTouch() const
    {
        return allowAButtonTouch;
    }

    static View* create();

  private:
    void refillHints(View* focusView);
    bool addUnableAButtonAction = true;
    bool allowAButtonTouch      = false;
    bool forceShown             = false;

    VoidEvent::Subscription hintSubscription;
#if defined(PLATFORM_XBOX360)
    // 按按钮缓存已创建的 Hint（每个按键最多一个，与 refill 的
    // addedButtons 去重一致）；~Hints 统一释放。
    std::map<ControllerButton, Hint*> hintPool;
#endif
};

} // namespace brls
