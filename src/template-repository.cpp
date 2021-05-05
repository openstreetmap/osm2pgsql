/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2021 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "template-repository.hpp"

#include <osmium/util/string.hpp>

#include <cassert>
#include <stdexcept>

void template_repository_t::add(std::string const &name,
                                std::string const &content)
{
    assert(!name.empty());

    if (name[0] != '.') {
        m_templates[name] = content;
    } else {
        for (auto const *const table : {"nodes", "ways", "relations"}) {
            m_templates[table + name] = content;
        }
    }
}

std::string template_repository_t::operator()(std::string const &name) const
{
    auto const it = m_templates.find(name);
    if (it == m_templates.end()) {
        throw std::runtime_error{"Missing template '{}'"_format(name)};
    }

    fmt::dynamic_format_arg_store<fmt::format_context> args;
    for (auto const &p : m_vars) {
        args.push_back(fmt::arg(p.first.c_str(), p.second));
    }

    auto const elements = osmium::split_string(name, '.');
    args.push_back(fmt::arg("table", elements[0]));

    return fmt::vformat(it->second, args);
}

