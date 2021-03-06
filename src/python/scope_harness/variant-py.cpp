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
 * Author: Pawel Stolowski <pawel.stolowski@canonical.com>
 */

#include <boost/python.hpp>
#include <boost/python/stl_iterator.hpp>
#include <unity/scopes/Variant.h>
#include <unity/UnityExceptions.h>

using namespace boost::python;
namespace us = unity::scopes;

struct VariantToPythonObj
{
    static PyObject* convert(const unity::scopes::Variant& v)
    {
        switch (v.which())
        {
            case us::Variant::Null:
                return incref(object().ptr());
            case us::Variant::Int:
                return incref(object(v.get_int()).ptr());
            case us::Variant::Bool:
                return incref(object(v.get_bool()).ptr());
            case us::Variant::String:
                return incref(object(v.get_string()).ptr());
            case us::Variant::Double:
                return incref(object(v.get_double()).ptr());
            case us::Variant::Dict:
                {
                    dict pydict;
                    for (auto el: v.get_dict())
                    {
                        pydict[el.first] = el.second;
                    }
                    return incref(pydict.ptr());
                }
                break;
            case us::Variant::Array:
                {
                    list pylist;
                    for (auto const el: v.get_array())
                    {
                        pylist.append(el);
                    }
                    return incref(pylist.ptr());
                }
                break;
            default:
                throw unity::InvalidArgumentException("Unsupported variant type");
        }
    }
};

struct VariantFromPythonObj
{
    static void* convertible(PyObject *obj)
    {
        if (PyUnicode_Check(obj) ||
                PyBytes_Check(obj) ||
                PyLong_Check(obj) ||
                PyFloat_Check(obj) ||
                PyBool_Check(obj) ||
                PyList_Check(obj) ||
                PyDict_Check(obj) ||
                obj == Py_None)
            return obj;
        return nullptr;
    }

    static void construct(PyObject* obj, boost::python::converter::rvalue_from_python_stage1_data* data)
    {
        void* storage = ((boost::python::converter::rvalue_from_python_storage<us::Variant>*) data)->storage.bytes;

        //
        // the boost-python'ish way of extracting python object values is to do:
        //   extract<int> val(obj);
        //   if (val.check()) {
        //     int_val = val();
        //   }
        // but that way we cannot distinguish between floats and integers (extract will
        // happily convert between both), so use python C API directly to check real data type, which
        // is also more efficient for multiple checks.
        //
        if (PyBool_Check(obj))
        {
            new (storage) us::Variant(extract<bool>(obj));
        }
        else if (PyUnicode_Check(obj))
        {
            new (storage) us::Variant(extract<std::string>(obj));
        }
        else if (PyBytes_Check(obj)) //TODO: is this needed?
        {
            new (storage) us::Variant(extract<std::string>(obj));
        }
        else if (PyLong_Check(obj))
        {
            // FIXME: this can overflow when converted to int
            new (storage) us::Variant(extract<int>(obj));
        }
        else if (PyFloat_Check(obj))
        {
            new (storage) us::Variant(extract<double>(obj));
        }
        else if (PyList_Check(obj))
        {
            us::VariantArray va;
            auto lst = extract<list>(obj);
            stl_input_iterator<object> begin(lst), end;
            for (auto it = begin; it != end; ++it)
            {
                va.push_back(extract<unity::scopes::Variant>(*it));
            }
            new (storage) us::Variant(va);
        }
        else if (PyDict_Check(obj))
        {
            us::VariantMap vm;
            dict d = extract<dict>(obj);
            stl_input_iterator<object> begin(d.items()), end;
            for (auto it = begin; it != end; ++it)
            {
                vm[extract<std::string>((*it)[0])] = extract<unity::scopes::Variant>((*it)[1]);
            }
            new (storage) us::Variant(vm);
        }
        else // null variant
        {
            new (storage) us::Variant();
        }

        data->convertible = storage;
    }
};
 
void export_variant()
{
    // register Variant -> python object converter
    boost::python::to_python_converter<unity::scopes::Variant, VariantToPythonObj>();

    // register python object -> Variant converter
    boost::python::converter::registry::push_back(
        &VariantFromPythonObj::convertible,
        &VariantFromPythonObj::construct,
        boost::python::type_id<us::Variant>());
}
