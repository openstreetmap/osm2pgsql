#include <algorithm>
#include <cstring>

#include <boost/property_tree/json_parser.hpp>
#include <osmium/osm.hpp>

#include "gazetteer-style.hpp"
#include "pgsql.hpp"
#include "wkb.hpp"

enum : int
{
    MAX_ADMINLEVEL = 15
};

static std::vector<osmium::Tag const *>
domain_names(char const *cls, osmium::TagList const &tags)
{
    std::vector<osmium::Tag const *> ret;

    std::string const prefix = cls + std::string(":name");
    auto plen = prefix.length();

    for (auto const &item : tags) {
        char const *k = item.key();
        if (prefix.compare(0, plen, k) == 0 &&
            (k[plen] == '\0' || k[plen] == ':')) {
            ret.push_back(&item);
        }
    }

    return ret;
}

namespace pt = boost::property_tree;

static auto place_table =
    std::make_shared<db_target_descr_t>("place", "place_id");

void gazetteer_style_t::clear()
{
    m_main.clear();
    m_names.clear();
    m_extra.clear();
    m_address.clear();
    m_operator = nullptr;
    m_admin_level = MAX_ADMINLEVEL;
    m_is_named = false;
}

bool gazetteer_style_t::has_place(std::string const &cls) const
{
    return std::any_of(m_main.begin(), m_main.end(), [&](pmaintag_t const &e) {
        if (strcmp(std::get<0>(e), cls.c_str()) == 0) {
            if (std::get<2>(e) & SF_MAIN_NAMED)
                return !m_names.empty();
            // XXX should handle SF_MAIN_NAMED_KEY as well

            return true;
        }

        return false;
    });
}

void gazetteer_style_t::load_style(std::string const &filename)
{
    fprintf(stderr, "Parsing gazetteer style file '%s'.\n", filename.c_str());
    pt::ptree root;

    pt::read_json(filename, root);

    for (auto &entry : root) {
        for (auto &tag : entry.second.get_child("keys")) {
            for (auto &value : entry.second.get_child("values")) {
                add_style_entry(tag.second.data(), value.first,
                                parse_flags(value.second.data()));
            }
        }
    }
}

gazetteer_style_t::flag_t gazetteer_style_t::parse_flags(std::string const &str)
{
    flag_t out = 0;

    std::string::size_type start = 0;

    while (start != std::string::npos) {
        auto end = str.find(',', start);

        std::string item;

        if (end == std::string::npos) {
            item = str.substr(start);
            start = std::string::npos;
        } else {
            item = str.substr(start, end - start);
            start = end + 1;
        }

        if (item == "skip") {
            return 0;
        } else if (item == "main") {
            out |= SF_MAIN;
        } else if (item == "with_name_key") {
            out |= SF_MAIN_NAMED_KEY;
        } else if (item == "with_name") {
            out |= SF_MAIN_NAMED;
        } else if (item == "fallback") {
            out |= SF_MAIN_FALLBACK;
        } else if (item == "operator") {
            out |= SF_MAIN_OPERATOR;
            m_any_operator_matches = true;
        } else if (item == "name") {
            out |= SF_NAME;
        } else if (item == "ref") {
            out |= SF_REF;
        } else if (item == "address") {
            out |= SF_ADDRESS;
        } else if (item == "house") {
            out |= SF_ADDRESS_POINT;
        } else if (item == "postcode") {
            out |= SF_POSTCODE;
        } else if (item == "country") {
            out |= SF_COUNTRY;
        } else if (item == "extra") {
            out |= SF_EXTRA;
        } else if (item == "interpolation") {
            out |= SF_INTERPOLATION;
        } else {
            throw std::runtime_error("Unknown flag in style file.");
        }
    }

    return out;
}

bool gazetteer_style_t::add_metadata_style_entry(std::string const &key)
{
    if (key == "osm_version") {
        m_metadata_fields.set_version(true);
    } else if (key == "osm_timestamp") {
        m_metadata_fields.set_timestamp(true);
    } else if (key == "osm_changeset") {
        m_metadata_fields.set_changeset(true);
    } else if (key == "osm_uid") {
        m_metadata_fields.set_uid(true);
    } else if (key == "osm_user") {
        m_metadata_fields.set_user(true);
    } else {
        return false;
    }
    return true;
}

void gazetteer_style_t::add_style_entry(std::string const &key,
                                        std::string const &value,
                                        gazetteer_style_t::flag_t flags)
{
    if (key.empty()) {
        if (value.empty()) {
            m_default = flags;
        } else {
            m_matcher.emplace_back(value, flags, matcher_t::MT_VALUE);
        }
        return;
    }

    // prefix: works on empty key only
    if (key[key.size() - 1] == '*') {
        if (key.size() == 1) {
            throw std::runtime_error("Style error. Ambiguous key '*'.");
        }
        if (!value.empty()) {
            throw std::runtime_error(
                "Style error. Prefix key can only be used with empty value.\n");
        }
        m_matcher.emplace_back(key.substr(0, key.size() - 1), flags,
                               matcher_t::MT_PREFIX);
        return;
    }

    // suffix: dito
    if (key[0] == '*') {
        if (!value.empty()) {
            throw std::runtime_error(
                "Style error. Suffix key can only be used with empty value.\n");
        }
        m_matcher.emplace_back(key.substr(1), flags, matcher_t::MT_SUFFIX);
        return;
    }

    if (key == "boundary") {
        if (value.empty() || value == "administrative") {
            flags |= SF_BOUNDARY;
        }
    }

    if (add_metadata_style_entry(key)) {
        if (!value.empty()) {
            throw std::runtime_error("Style error. Rules for OSM metadata "
                                     "attributes must have an empty value.\n");
        }
        if (flags != SF_EXTRA) {
            throw std::runtime_error("Style error. Rules for OSM metadata "
                                     "attributes must have the style flag "
                                     "\"extra\" and no other flag.\n");
        }
        return;
    }
    if (value.empty()) {
        m_matcher.emplace_back(key, flags, matcher_t::MT_KEY);
    } else {
        m_matcher.emplace_back(key + '\0' + value, flags, matcher_t::MT_FULL);
    }
}

gazetteer_style_t::flag_t gazetteer_style_t::find_flag(char const *k,
                                                       char const *v) const
{
    auto klen = std::strlen(k);
    auto vlen = std::strlen(v);

    // full match
    auto fulllen = klen + vlen + 1U;
    for (auto const &e : m_matcher) {
        switch (e.type) {
        case matcher_t::MT_FULL:
            if (e.name.size() == fulllen && strcmp(k, e.name.c_str()) == 0 &&
                memcmp(v, e.name.data() + klen + 1, vlen) == 0) {
                return e.flag;
            }
            break;
        case matcher_t::MT_KEY:
            if (e.name.size() == klen && memcmp(k, e.name.data(), klen) == 0) {
                return e.flag;
            }
            break;
        case matcher_t::MT_PREFIX:
            if (e.name.size() < klen &&
                memcmp(k, e.name.data(), e.name.size()) == 0) {
                return e.flag;
            }
            break;
        case matcher_t::MT_SUFFIX:
            if (e.name.size() < klen &&
                memcmp(k + klen - e.name.size(), e.name.data(),
                       e.name.size()) == 0) {
                return e.flag;
            }
            break;
        case matcher_t::MT_VALUE:
            if (e.name.size() == vlen && memcmp(v, e.name.data(), vlen) == 0) {
                return e.flag;
            }
            break;
        }
    }

    return m_default;
}

void gazetteer_style_t::process_tags(osmium::OSMObject const &o)
{
    clear();

    char const *postcode = nullptr;
    char const *country = nullptr;
    char const *place = nullptr;
    flag_t place_flag;
    bool address_point = false;
    bool interpolation = false;
    bool admin_boundary = false;
    bool postcode_fallback = false;

    for (auto const &item : o.tags()) {
        char const *k = item.key();
        char const *v = item.value();

        if (strcmp(k, "admin_level") == 0) {
            m_admin_level = atoi(v);
            if (m_admin_level <= 0 || m_admin_level > MAX_ADMINLEVEL)
                m_admin_level = MAX_ADMINLEVEL;
            continue;
        }

        if (m_any_operator_matches && strcmp(k, "operator") == 0) {
            m_operator = v;
        }

        flag_t flag = find_flag(k, v);

        if (flag == 0) {
            continue;
        }

        if (flag & SF_MAIN) {
            if (strcmp(k, "place") == 0) {
                place = v;
                place_flag = flag;
            } else {
                m_main.emplace_back(k, v, flag);
                if ((flag & SF_BOUNDARY) && strcmp(v, "administrative") == 0) {
                    admin_boundary = true;
                }
            }
        }

        if (flag & (SF_NAME | SF_REF)) {
            m_names.emplace_back(k, v);
            if (flag & SF_NAME) {
                m_is_named = true;
            }
        }

        if (flag & SF_ADDRESS) {
            char const *addr_key;
            if (strncmp(k, "addr:", 5) == 0) {
                addr_key = k + 5;
            } else if (strncmp(k, "is_in:", 6) == 0) {
                addr_key = k + 6;
            } else {
                addr_key = k;
            }

            if (strcmp(addr_key, "postcode") == 0) {
                if (!postcode) {
                    postcode = v;
                }
            } else if (strcmp(addr_key, "country") == 0) {
                if (!country && strlen(v) == 2) {
                    country = v;
                }
            } else {
                bool first = std::none_of(
                    m_address.begin(), m_address.end(), [&](ptag_t const &t) {
                        return strcmp(t.first, addr_key) == 0;
                    });
                if (first) {
                    m_address.emplace_back(addr_key, v);
                }
            }
        }

        if (flag & SF_ADDRESS_POINT) {
            address_point = true;
            m_is_named = true;
        }

        if ((flag & SF_POSTCODE) && !postcode) {
            postcode = v;
            if (flag & SF_MAIN_FALLBACK) {
                postcode_fallback = true;
            }
        }

        if ((flag & SF_COUNTRY) && !country && std::strlen(v) == 2) {
            country = v;
        }

        if (flag & SF_EXTRA) {
            m_extra.emplace_back(k, v);
        }

        if (flag & SF_INTERPOLATION) {
            m_main.emplace_back("place", "houses", SF_MAIN);
            interpolation = true;
        }
    }

    if (postcode) {
        m_address.emplace_back("postcode", postcode);
    }
    if (country) {
        m_address.emplace_back("country", country);
    }
    if (place) {
        if (interpolation || (admin_boundary &&
                              strncmp(place, "isl", 3) != 0)) // island or islet
            m_extra.emplace_back("place", place);
        else
            m_main.emplace_back("place", place, place_flag);
    }
    if (address_point) {
        m_main.emplace_back("place", "house", SF_MAIN | SF_MAIN_FALLBACK);
    } else if (postcode_fallback && postcode) {
        m_main.emplace_back("place", "postcode", SF_MAIN | SF_MAIN_FALLBACK);
    }
}

bool gazetteer_style_t::copy_out(osmium::OSMObject const &o,
                                 std::string const &geom, db_copy_mgr_t &buffer)
{
    bool any = false;
    for (auto const &main : m_main) {
        if (!(std::get<2>(main) & SF_MAIN_FALLBACK)) {
            any |= copy_out_maintag(main, o, geom, buffer);
        }
    }

    if (any)
        return true;

    for (auto const &main : m_main) {
        if ((std::get<2>(main) & SF_MAIN_FALLBACK) &&
            copy_out_maintag(main, o, geom, buffer)) {
            return true;
        }
    }

    return false;
}

bool gazetteer_style_t::copy_out_maintag(pmaintag_t const &tag,
                                         osmium::OSMObject const &o,
                                         std::string const &geom,
                                         db_copy_mgr_t &buffer)
{
    std::vector<osmium::Tag const *> domain_name;
    if (std::get<2>(tag) & SF_MAIN_NAMED_KEY) {
        domain_name = domain_names(std::get<0>(tag), o.tags());
        if (domain_name.empty())
            return false;
    }

    if (std::get<2>(tag) & SF_MAIN_NAMED) {
        if (domain_name.empty() && !m_is_named) {
            return false;
        }
    }

    buffer.new_line(place_table);
    // osm_id
    buffer.add_column(o.id());
    // osm_type
    char const osm_type[2] = { (char)toupper(osmium::item_type_to_char(o.type())), '\0'};
    buffer.add_column(osm_type);
    // class
    buffer.add_column(std::get<0>(tag));
    // type
    buffer.add_column(std::get<1>(tag));
    // names
    if (!domain_name.empty()) {
        auto prefix_len = strlen(std::get<0>(tag)) + 1; // class name and ':'
        buffer.new_hash();
        for (auto *t : domain_name) {
            buffer.add_hash_elem(t->key() + prefix_len, t->value());
        }
        buffer.finish_hash();
    } else {
        bool first = true;
        // operator will be ignored on anything but these classes
        if (m_operator && (std::get<2>(tag) & SF_MAIN_OPERATOR)) {
            buffer.new_hash();
            buffer.add_hash_elem("operator", m_operator);
            first = false;
        }
        for (auto const &entry : m_names) {
            if (first) {
                buffer.new_hash();
                first = false;
            }

            buffer.add_hash_elem(entry.first, entry.second);
        }

        if (first) {
            buffer.add_null_column();
        } else {
            buffer.finish_hash();
        }
    }
    // admin_level
    buffer.add_column(m_admin_level);
    // address
    if (m_address.empty()) {
        buffer.add_null_column();
    } else {
        buffer.new_hash();
        for (auto const &a : m_address) {
            if (strcmp(a.first, "tiger:county") == 0) {
                std::string term;
                auto *end = strchr(a.second, ',');
                if (end) {
                    auto len = (std::string::size_type)(end - a.second);
                    term = std::string(a.second, len);
                } else {
                    term = a.second;
                }
                term += " county";
                buffer.add_hash_elem(a.first, term);
            } else {
                buffer.add_hash_elem(a.first, a.second);
            }
        }
        buffer.finish_hash();
    }
    // extra tags
    if (m_extra.empty() && m_metadata_fields.none()) {
        buffer.add_null_column();
    } else {
        buffer.new_hash();
        for (auto const &entry : m_extra) {
            buffer.add_hash_elem(entry.first, entry.second);
        }
        if (m_metadata_fields.version() && o.version()) {
            buffer.add_hstore_num_noescape<osmium::object_version_type>(
                "osm_version", o.version());
        }
        if (m_metadata_fields.uid() && o.uid()) {
            buffer.add_hstore_num_noescape<osmium::user_id_type>("osm_uid", o.uid());
        }
        if (m_metadata_fields.user() && o.user() && *(o.user()) != '\0') {
            buffer.add_hash_elem("osm_user", o.user());
        }
        if (m_metadata_fields.changeset() && o.changeset()) {
            buffer.add_hstore_num_noescape<osmium::changeset_id_type>(
                "osm_changeset", o.changeset());
        }
        if (m_metadata_fields.timestamp() && o.timestamp()) {
            std::string timestamp = o.timestamp().to_iso();
            buffer.add_hash_elem_noescape("osm_timestamp", timestamp.c_str());
        }
        buffer.finish_hash();
    }
    // add the geometry - encoding it to hex along the way
    buffer.add_hex_geom(geom);

    buffer.finish_line();

    return true;
}