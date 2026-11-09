#include "fp3d/worldgen.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "fp3d/bridge.h"
#include "json.h"
#include "json_loader.h"
#include "mod_manager.h"
#include "translation.h"
#include "worldfactory.h"

// options.h makes this a friend of cOpt: the bounds and choices are private there.
struct fp3d_option_access {
    static int imin( const options_manager::cOpt &o ) {
        return o.iMin;
    }
    static int imax( const options_manager::cOpt &o ) {
        return o.iMax;
    }
    static float fmin( const options_manager::cOpt &o ) {
        return o.fMin;
    }
    static float fmax( const options_manager::cOpt &o ) {
        return o.fMax;
    }
    static float fstep( const options_manager::cOpt &o ) {
        return o.fStep;
    }
    static int max_length( const options_manager::cOpt &o ) {
        return o.iMaxLength;
    }
    static const std::vector<options_manager::int_and_option> &int_values( const options_manager::cOpt &o ) {
        return o.mIntValues;
    }
    static void set_int( options_manager::cOpt &o, int v ) {
        o.iSet = v;
    }
};

namespace fp3d
{
namespace
{
constexpr size_t max_mods = 512;
constexpr size_t max_string_option = 256;

bool shown( const options_manager::cOpt &o )
{
    const std::string t = o.getType();
    return !o.is_hidden() && ( t == "bool" || t == "int" || t == "int_map" || t == "float" ||
                               t == "string_select" || t == "string_input" );
}

const std::vector<mod_id> &usable_mods()
{
    return world_generator->get_mod_manager().get_usable_mods();
}

bool usable( const mod_id &m )
{
    const std::vector<mod_id> &u = usable_mods();
    return std::find( u.begin(), u.end(), m ) != u.end() && m.is_valid() && !m->obsolete;
}

void write_ids( JsonOut &jo, const char *name, const std::vector<mod_id> &v )
{
    jo.member( name );
    jo.start_array();
    for( const mod_id &m : v ) {
        jo.write( m.str() );
    }
    jo.end_array();
}

// Adds `m` after its dependencies (depth first). False if a dependency isn't usable or there's a cycle.
bool add_with_deps( const mod_id &m, std::vector<mod_id> &out, std::set<mod_id> &visiting, std::string &why )
{
    if( std::find( out.begin(), out.end(), m ) != out.end() ) {
        return true;
    }
    if( !usable( m ) ) {
        why = "mod not available: " + sanitize_text( m.str(), 64 );
        return false;
    }
    if( !visiting.insert( m ).second || visiting.size() > max_mods ) {
        why = "mod dependency loop at " + m.str();
        return false;
    }
    for( const mod_id &d : m->dependencies ) {
        if( !add_with_deps( d, out, visiting, why ) ) {
            return false;
        }
    }
    visiting.erase( m );
    out.push_back( m );
    return true;
}
} // namespace

std::string world_setup_catalog_json()
{
    const options_manager::options_container defaults = get_options().get_world_defaults();
    std::vector<const options_manager::cOpt *> opts;
    for( const auto &kv : defaults ) {
        if( shown( kv.second ) ) {
            opts.push_back( &kv.second );
        }
    }
    std::sort( opts.begin(), opts.end(), []( const options_manager::cOpt * a, const options_manager::cOpt * b ) {
        return a->getMenuText() < b->getMenuText();
    } );

    std::ostringstream os;
    JsonOut jo( os );
    jo.start_object();
    jo.member( "version", 1 );
    jo.member( "options" );
    jo.start_array();
    for( const options_manager::cOpt *o : opts ) {
        const std::string type = o->getType();
        jo.start_object();
        jo.member( "id", o->getName() );
        jo.member( "name", o->getMenuText() );
        jo.member( "tooltip", o->getTooltip() );
        jo.member( "type", type );
        if( type == "bool" ) {
            jo.member( "value", o->value_as<bool>() );
        } else if( type == "int" ) {
            jo.member( "value", o->value_as<int>() );
            jo.member( "min", fp3d_option_access::imin( *o ) );
            jo.member( "max", fp3d_option_access::imax( *o ) );
        } else if( type == "int_map" ) {
            jo.member( "value", o->value_as<int>() );
            jo.member( "choices" );
            jo.start_array();
            for( const options_manager::int_and_option &c : fp3d_option_access::int_values( *o ) ) {
                jo.start_array();
                jo.write( c.first );
                jo.write( c.second.translated() );
                jo.end_array();
            }
            jo.end_array();
        } else if( type == "float" ) {
            jo.member( "value", o->value_as<float>() );
            jo.member( "min", fp3d_option_access::fmin( *o ) );
            jo.member( "max", fp3d_option_access::fmax( *o ) );
            jo.member( "step", fp3d_option_access::fstep( *o ) );
        } else if( type == "string_select" ) {
            jo.member( "value", o->getValue() );
            jo.member( "choices" );
            jo.start_array();
            for( const options_manager::id_and_option &c : o->getItems() ) {
                jo.start_array();
                jo.write( c.first );
                jo.write( c.second.translated() );
                jo.end_array();
            }
            jo.end_array();
        } else {
            jo.member( "value", o->getValue() );
            jo.member( "max_length", static_cast<int>( std::min<size_t>( max_string_option,
                       fp3d_option_access::max_length( *o ) > 0 ? fp3d_option_access::max_length( *o ) : max_string_option ) ) );
        }
        jo.end_object();
    }
    jo.end_array();

    // CDDA's "Create World" sliders (city size, spawns, ...): each level sets several hidden options.
    jo.member( "sliders" );
    jo.start_array();
    for( const option_slider &osl : option_slider::get_all() ) {
        if( osl.context() != "WORLDGEN" ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", osl.id.str() );
        jo.member( "name", osl.name().translated() );
        jo.member( "default", osl.default_level() );
        jo.member( "levels" );
        jo.start_array();
        for( int i = 0; i < osl.count(); ++i ) {
            jo.start_object();
            jo.member( "name", osl.level_name( i ).translated() );
            jo.member( "description", osl.level_desc( i ).translated() );
            jo.end_object();
        }
        jo.end_array();
        jo.end_object();
    }
    jo.end_array();

    jo.member( "mods" );
    jo.start_array();
    for( const mod_id &m : usable_mods() ) {
        if( !m.is_valid() || m->obsolete ) {
            continue;
        }
        jo.start_object();
        jo.member( "id", m.str() );
        jo.member( "name", m->name() );
        jo.member( "description", m->description.translated() );
        jo.member( "category", m->category.second.translated() );
        jo.member( "core", m->core );
        write_ids( jo, "dependencies", m->dependencies );
        write_ids( jo, "conflicts", m->conflicts );
        jo.end_object();
    }
    jo.end_array();
    write_ids( jo, "default_mods", world_generator->get_mod_manager().get_default_mods() );
    jo.end_object();
    return os.str();
}

std::string apply_world_setup( const std::string &json, options_manager::options_container &options,
                               std::vector<mod_id> &mods )
{
    options = get_options().get_world_defaults();
    mods.clear();
    if( json.empty() || json.size() > max_answer_json ) {
        return "no settings";
    }
    try {
        const JsonValue jv = json_loader::from_string( json );
        const JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        // Sliders first (every one, at its default unless chosen, as CDDA's screen does); options override.
        std::map<std::string, int> levels;
        if( jo.has_object( "sliders" ) ) {
            const JsonObject so = jo.get_object( "sliders" );
            so.allow_omitted_members();
            size_t n = 0;
            for( const JsonMember m : so ) {
                if( ++n > 256 || !m.test_int() ) {
                    return "bad slider";
                }
                levels[m.name()] = m.get_int();
            }
        }
        size_t used = 0;
        for( const option_slider &osl : option_slider::get_all() ) {
            if( osl.context() != "WORLDGEN" ) {
                continue;
            }
            int level = osl.default_level();
            auto it = levels.find( osl.id.str() );
            if( it != levels.end() ) {
                if( it->second < 0 || it->second >= osl.count() ) {
                    return "bad level for " + osl.name().translated();
                }
                level = it->second;
                ++used;
            }
            osl.apply_opts( level, options );
        }
        if( used != levels.size() ) {
            return "unknown slider";
        }
        if( jo.has_object( "options" ) ) {
            const JsonObject oo = jo.get_object( "options" );
            oo.allow_omitted_members();
            size_t n = 0;
            for( const JsonMember m : oo ) {
                if( ++n > 1024 ) {
                    return "too many settings";
                }
                const std::string id = m.name();
                auto it = options.find( id );
                if( it == options.end() || !shown( it->second ) ) {
                    return "unknown world setting: " + sanitize_text( id, 64 );
                }
                options_manager::cOpt &o = it->second;
                const std::string type = o.getType();
                const std::string bad = "bad value for " + o.getMenuText();
                if( type == "bool" ) {
                    if( !m.test_bool() ) {
                        return bad;
                    }
                    o.setValue( m.get_bool() ? "true" : "false" );
                } else if( type == "int" ) {
                    if( !m.test_int() ) {
                        return bad;
                    }
                    const int v = m.get_int();
                    if( v < fp3d_option_access::imin( o ) || v > fp3d_option_access::imax( o ) ) {
                        return bad;
                    }
                    o.setValue( v );
                } else if( type == "int_map" ) {
                    if( !m.test_int() ) {
                        return bad;
                    }
                    const int v = m.get_int();
                    const auto &vals = fp3d_option_access::int_values( o );
                    if( std::none_of( vals.begin(), vals.end(), [v]( const options_manager::int_and_option & c ) {
                    return c.first == v;
                } ) ) {
                        return bad;
                    }
                    fp3d_option_access::set_int( o, v );
                } else if( type == "float" ) {
                    if( !m.test_number() ) {
                        return bad;
                    }
                    const double v = m.get_float();
                    if( !std::isfinite( v ) || v < fp3d_option_access::fmin( o ) || v > fp3d_option_access::fmax( o ) ) {
                        return bad;
                    }
                    o.setValue( static_cast<float>( v ) );
                } else if( type == "string_select" ) {
                    if( !m.test_string() || o.getItemPos( m.get_string() ) < 0 ) {
                        return bad;
                    }
                    o.setValue( m.get_string() );
                } else {
                    if( !m.test_string() ) {
                        return bad;
                    }
                    o.setValue( sanitize_text( m.get_string(), max_string_option ) );
                }
            }
        }

        std::vector<mod_id> picked;
        if( jo.has_array( "mods" ) ) {
            for( const std::string &id : jo.get_string_array( "mods" ) ) {
                if( picked.size() >= max_mods ) {
                    return "too many mods";
                }
                const mod_id m( id );
                if( !usable( m ) ) {
                    return "mod not available: " + sanitize_text( id, 64 );
                }
                picked.push_back( m );
            }
        } else {
            picked = world_generator->get_mod_manager().get_default_mods();
        }
        // Core mods first (CDDA loads them before any other), then the rest, each after its dependencies.
        std::stable_partition( picked.begin(), picked.end(), []( const mod_id & m ) {
            return m.is_valid() && m->core;
        } );
        std::string why;
        for( const mod_id &m : picked ) {
            std::set<mod_id> visiting;
            if( !add_with_deps( m, mods, visiting, why ) ) {
                return why;
            }
        }
        if( std::count_if( mods.begin(), mods.end(), []( const mod_id & m ) {
        return m->core;
    } ) != 1 ) {
            return "pick exactly one core mod (e.g. Dark Days Ahead)";
        }
        for( const mod_id &a : mods ) {
            for( const mod_id &c : a->conflicts ) {
                if( std::find( mods.begin(), mods.end(), c ) != mods.end() ) {
                    return a->name() + " conflicts with " + c->name();
                }
            }
        }
    } catch( const std::exception &e ) {
        return std::string( "bad settings: " ) + sanitize_text( e.what(), 200 );
    }
    return "";
}

WORLD *run_world_setup( const std::string &name )
{
    const std::string catalog = world_setup_catalog_json();
    std::string status;
    for( ;; ) {
        ui_request req;
        req.kind = request_kind::world_setup;
        req.context = "WORLDGEN";
        req.title = "New world: " + name;
        req.text = catalog;
        req.status = status;
        req.allow_cancel = true;
        const ui_answer a = ask( req );
        if( a.cancel ) {
            return nullptr;
        }
        options_manager::options_container options;
        std::vector<mod_id> mods;
        status = apply_world_setup( a.text, options, mods );
        if( !status.empty() ) {
            continue;
        }
        WORLD *world = world_generator->make_new_world( name, mods );
        if( world == nullptr ) {
            throw std::runtime_error( "make_new_world failed" );
        }
        world->WORLD_OPTIONS = options;
        if( !world->save() ) {
            throw std::runtime_error( "cannot save the world's settings" );
        }
        return world;
    }
}
} // namespace fp3d
