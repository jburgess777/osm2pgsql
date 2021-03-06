extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

#include <boost/format.hpp>

#include "options.hpp"
#include "tagtransform-lua.hpp"

lua_tagtransform_t::lua_tagtransform_t(options_t const *options)
: L(luaL_newstate()), m_node_func(options->tag_transform_node_func.get_value_or(
                          "filter_tags_node")),
  m_way_func(options->tag_transform_way_func.get_value_or("filter_tags_way")),
  m_rel_func(
      options->tag_transform_rel_func.get_value_or("filter_basic_tags_rel")),
  m_rel_mem_func(options->tag_transform_rel_mem_func.get_value_or(
      "filter_tags_relation_member")),
  m_extra_attributes(options->extra_attributes)
{
    luaL_openlibs(L);
    if (luaL_dofile(L, options->tag_transform_script->c_str())) {
        throw std::runtime_error(
            (boost::format("Lua tag transform style error: %1%") %
             lua_tostring(L, -1))
                .str());
    }

    check_lua_function_exists(m_node_func);
    check_lua_function_exists(m_way_func);
    check_lua_function_exists(m_rel_func);
    check_lua_function_exists(m_rel_mem_func);
}

lua_tagtransform_t::~lua_tagtransform_t() { lua_close(L); }

void lua_tagtransform_t::check_lua_function_exists(const std::string &func_name)
{
    lua_getglobal(L, func_name.c_str());
    if (!lua_isfunction(L, -1)) {
        throw std::runtime_error(
            (boost::format(
                 "Tag transform style does not contain a function %1%") %
             func_name)
                .str());
    }
    lua_pop(L, 1);
}

bool lua_tagtransform_t::filter_tags(osmium::OSMObject const &o, int *polygon,
                                     int *roads, export_list const &,
                                     taglist_t &out_tags, bool)
{
    switch (o.type()) {
    case osmium::item_type::node:
        lua_getglobal(L, m_node_func.c_str());
        break;
    case osmium::item_type::way:
        lua_getglobal(L, m_way_func.c_str());
        break;
    case osmium::item_type::relation:
        lua_getglobal(L, m_rel_func.c_str());
        break;
    default:
        throw std::runtime_error("Unknown OSM type");
    }

    lua_newtable(L); /* key value table */

    lua_Integer sz = 0;
    for (auto const &t : o.tags()) {
        lua_pushstring(L, t.key());
        lua_pushstring(L, t.value());
        lua_rawset(L, -3);
        ++sz;
    }
    if (m_extra_attributes && o.version() > 0) {
        taglist_t tags;
        tags.add_attributes(o);
        for (auto const &t : tags) {
            lua_pushstring(L, t.key.c_str());
            lua_pushstring(L, t.value.c_str());
            lua_rawset(L, -3);
        }
        sz += tags.size();
    }

    lua_pushinteger(L, sz);

    if (lua_pcall(L, 2, (o.type() == osmium::item_type::way) ? 4 : 2, 0)) {
        /* lua function failed */
        throw std::runtime_error(
            (boost::format(
                 "Failed to execute lua function for basic tag processing: %1%") %
             lua_tostring(L, -1))
                .str());
    }

    if (o.type() == osmium::item_type::way) {
        if (roads) {
            *roads = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        if (polygon) {
            *polygon = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        const char *key = lua_tostring(L, -2);
        const char *value = lua_tostring(L, -1);
        if (key == NULL) {
           int ltype = lua_type(L, -2);
           throw std::runtime_error(
               (boost::format(
                    "Basic tag processing returned NULL key. Possibly this is due an incorrect data type '%1%'.") %
                lua_typename(L, ltype))
                   .str());
        }
        if (value == NULL) {
           int ltype = lua_type(L, -1);
           throw std::runtime_error(
               (boost::format(
                    "Basic tag processing returned NULL value. Possibly this is due an incorrect data type '%1%'.") %
                lua_typename(L, ltype))
                   .str());
        }
        out_tags.emplace_back(key, value);
        lua_pop(L, 1);
    }

    bool filter = lua_tointeger(L, -2);

    lua_pop(L, 2);

    return filter;
}

bool lua_tagtransform_t::filter_rel_member_tags(
    taglist_t const &rel_tags, osmium::memory::Buffer const &members,
    rolelist_t const &member_roles, int *member_superseded, int *make_boundary,
    int *make_polygon, int *roads, export_list const &, taglist_t &out_tags,
    bool)
{
    size_t num_members = member_roles.size();
    lua_getglobal(L, m_rel_mem_func.c_str());

    lua_newtable(L); /* relations key value table */

    for (const auto &rel_tag : rel_tags) {
        lua_pushstring(L, rel_tag.key.c_str());
        lua_pushstring(L, rel_tag.value.c_str());
        lua_rawset(L, -3);
    }

    lua_newtable(L); /* member tags table */

    int idx = 1;
    for (auto const &w : members.select<osmium::Way>()) {
        lua_pushnumber(L, idx++);
        lua_newtable(L); /* member key value table */
        for (auto const &member_tag : w.tags()) {
            lua_pushstring(L, member_tag.key());
            lua_pushstring(L, member_tag.value());
            lua_rawset(L, -3);
        }
        lua_rawset(L, -3);
    }

    lua_newtable(L); /* member roles table */

    for (size_t i = 0; i < num_members; ++i) {
        lua_pushnumber(L, i + 1);
        lua_pushstring(L, member_roles[i]);
        lua_rawset(L, -3);
    }

    lua_pushnumber(L, num_members);

    if (lua_pcall(L, 4, 6, 0)) {
        /* lua function failed */
        throw std::runtime_error(
            (boost::format(
                 "Failed to execute lua function for relation tag processing: %1%") %
             lua_tostring(L, -1))
                .str());
    }

    *roads = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    *make_polygon = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    *make_boundary = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_pushnil(L);
    for (size_t i = 0; i < num_members; ++i) {
        if (lua_next(L, -2)) {
            member_superseded[i] = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        } else {
            throw std::runtime_error("Failed to read member_superseded from lua function");
        }
    }
    lua_pop(L, 2);

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        const char *key = lua_tostring(L, -2);
        const char *value = lua_tostring(L, -1);
        out_tags.push_back(tag_t(key, value));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    bool filter = lua_tointeger(L, -1);

    lua_pop(L, 1);

    return filter;
}
