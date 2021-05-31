#ifndef OSM2PGSQL_TEMPLATE_REPOSITORY_HPP
#define OSM2PGSQL_TEMPLATE_REPOSITORY_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2021 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "format.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Repository of templates and variables to fill those templates. Used for
 * building SQL commands.
 *
 * To use
 * - add templates with the `add()` function
 * - set variables with the `set()` function
 * and then get filled in templates with the call operator.
 */
class template_repository_t
{
public:
    /// Set template variable to the specified value.
    void set(std::string var, std::string value)
    {
        m_vars.emplace_back(std::move(var), std::move(value));
    }

    /**
     * Add a named template to the repository.
     *
     * If the name starts with a dot ('.'), this will add three templates
     * with the names prepended with "nodes", "ways", and "relations",
     * respectively.
     *
     * \pre \code !name.empty() \endcode
     */
    void add(std::string const &name, std::string const &content);

    /// Return filled in template or throw exception if it doesn't exist.
    std::string operator()(std::string const &name) const;

private:
    std::unordered_map<std::string, std::string> m_templates;
    std::vector<std::pair<std::string, std::string>> m_vars;
}; // class template_repository_t

#endif // OSM2PGSQL_TEMPLATE_REPOSITORY_HPP
