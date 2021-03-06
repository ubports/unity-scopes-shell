/*
 * Copyright (C) 2014 Canonical, Ltd.
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of version 3 of the GNU Lesser General Public License as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Pete Woods <pete.woods@canonical.com>
 */

#pragma once

#include <unity/scopes/Variant.h>

#include <vector>

#include <QtGlobal>

namespace unity
{
namespace scopeharness
{
namespace internal
{
class PreviewWidgetArguments;
}
namespace view
{
class AbstractView;
class PreviewView;
}
namespace preview
{

class Q_DECL_EXPORT PreviewWidget final
{
public:
    PreviewWidget(const PreviewWidget& other);

    PreviewWidget(PreviewWidget&& other);

    PreviewWidget& operator=(const PreviewWidget& other);

    PreviewWidget& operator=(PreviewWidget&& other);

    ~PreviewWidget();

    std::string id() const;

    std::string type() const;

    unity::scopes::Variant data() const;

    std::shared_ptr<view::AbstractView> trigger(const std::string& name, const unity::scopes::Variant& v);

protected:
    friend view::PreviewView;

    PreviewWidget(const internal::PreviewWidgetArguments& arguments);

    struct _Priv;

    std::shared_ptr<_Priv> p;
};

}
}
}
