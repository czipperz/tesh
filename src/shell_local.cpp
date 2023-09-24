#include "shell.hpp"

static cz::Str canonical_var(cz::Str key) {
#ifdef _WIN32
    if (key == "PATH")
        return "Path";
#endif
    return key;
}

bool get_var(const Shell_Local* local, cz::Str key, cz::Str* value) {
    for (; local; local = local->parent) {
        if (local->relationship == Shell_Local::ARGS_ONLY)
            continue;

        key = canonical_var(key);

        for (size_t i = 0; i < local->variable_names.len; ++i) {
            if (key == local->variable_names[i].str) {
                *value = local->variable_values[i].str;
                return true;
            }
        }

        // Unset variables should fail to lookup.
        for (size_t i = 0; i < local->unset_vars.len; ++i) {
            if (key == local->unset_vars[i].str)
                return false;
        }
    }

    return false;
}

void set_var(Shell_Local* local, cz::Str key, cz::Str value) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (key == local->variable_names[i].str) {
            local->variable_values[i].drop();
            local->variable_values[i] = RcStr::create_clone(value);
            return;
        }
    }

    // Setting a variable clears the unset property.
    for (size_t i = 0; i < local->unset_vars.len; ++i) {
        if (key == local->unset_vars[i].str) {
            local->unset_vars[i].drop();
            local->unset_vars.remove(i);
            return;
        }
    }

    local->variable_names.reserve(cz::heap_allocator(), 1);
    local->variable_values.reserve(cz::heap_allocator(), 1);
    local->variable_names.push(RcStr::create_clone(key));
    local->variable_values.push(RcStr::create_clone(value));
}

cz::Str get_wd(const Shell_Local* local) {
    for (; local; local = local->parent) {
        if (local->working_directories.len > 0) {
            return local->working_directories.last();
        }
    }
    return "";
}
bool get_old_wd(const Shell_Local* local, size_t num, cz::Str* result) {
    for (; local; local = local->parent) {
        if (num >= local->working_directories.len) {
            num -= local->working_directories.len;
        } else {
            size_t index = local->working_directories.len - num - 1;
            *result = local->working_directories[index];
            return true;
        }
    }
    return false;
}
void set_wd(Shell_Local* local, cz::Str value) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    // 128 should be enough.
    if (local->working_directories.len >= 128) {
        local->working_directories[0].drop(cz::heap_allocator());
        local->working_directories.remove(0);
    }

    local->working_directories.reserve(cz::heap_allocator(), 1);
    local->working_directories.push(value.clone_null_terminate(cz::heap_allocator()));
}

void make_env_var(Shell_Local* local, cz::Str key) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        if (key == local->exported_vars[i].str)
            return;
    }

    local->exported_vars.reserve(cz::heap_allocator(), 1);
    local->exported_vars.push(RcStr::create_clone(key));
}

void unset_var(Shell_Local* local, cz::Str key) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    key = canonical_var(key);

    for (size_t i = 0; i < local->variable_names.len; ++i) {
        if (local->variable_names[i].str == key) {
            local->variable_names[i].drop();
            local->variable_names.remove(i);
            local->variable_values.remove(i);
            break;
        }
    }
    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        if (local->exported_vars[i].str == key) {
            local->exported_vars[i].drop();
            local->exported_vars.remove(i);
            break;
        }
    }

    // If this is a forked subshell then we need to explicitly ignore
    // this variable to prevent lookups from continuing up the chain.
    if (local->parent) {
        // Double check the variable isn't already unset.
        for (size_t i = 0; i < local->unset_vars.len; ++i) {
            if (key == local->unset_vars[i].str) {
                return;
            }
        }

        local->unset_vars.reserve(cz::heap_allocator(), 1);
        local->unset_vars.push(RcStr::create_clone(key));
    }
}

int get_alias_or_function(const Shell_Local* local,
                          cz::Str alias_key,
                          cz::Str function_key,
                          Parse_Node** value) {
    bool allow_alias = true;
    for (const Shell_Local* elem = local; elem; elem = elem->parent) {
        if (alias_key == elem->blocked_alias) {
            allow_alias = false;
            break;
        }
    }

    for (; local; local = local->parent) {
        if (allow_alias) {
            for (size_t i = 0; i < local->alias_names.len; ++i) {
                if (local->alias_names[i] == alias_key) {
                    *value = local->alias_values[i];
                    return 1;
                }
            }
        }
        for (size_t i = 0; i < local->function_names.len; ++i) {
            if (local->function_names[i] == function_key) {
                *value = local->function_values[i];
                return 2;
            }
        }
    }
    return false;
}

Parse_Node* get_alias_no_recursion_check(const Shell_Local* local, cz::Str name) {
    for (const Shell_Local* elem = local; elem; elem = elem->parent) {
        for (size_t i = 0; i < elem->alias_names.len; ++i) {
            if (name == elem->alias_names[i]) {
                return elem->alias_values[i];
            }
        }
    }
    return nullptr;
}

Parse_Node* get_function(const Shell_Local* local, cz::Str name) {
    for (const Shell_Local* elem = local; elem; elem = elem->parent) {
        for (size_t i = 0; i < elem->function_names.len; ++i) {
            if (name == elem->function_names[i]) {
                return elem->function_values[i];
            }
        }
    }
    return nullptr;
}

void set_alias(Shell_Local* local, cz::Str key, Parse_Node* node) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    for (size_t i = 0; i < local->alias_names.len; ++i) {
        if (local->alias_names[i] == key) {
            // TODO: deallocate old node.
            local->alias_values[i] = node;
            return;
        }
    }

    local->alias_names.reserve(cz::heap_allocator(), 1);
    local->alias_values.reserve(cz::heap_allocator(), 1);
    // TODO: garbage collect / ref count?
    local->alias_names.push(key.clone(cz::heap_allocator()));
    local->alias_values.push(node);
}

void set_function(Shell_Local* local, cz::Str key, Parse_Node* node) {
    while (local && local->relationship == Shell_Local::ARGS_ONLY) {
        local = local->parent;
    }

    for (size_t i = 0; i < local->function_names.len; ++i) {
        if (local->function_names[i] == key) {
            // TODO: deallocate old node.
            local->function_values[i] = node;
            return;
        }
    }

    local->function_names.reserve(cz::heap_allocator(), 1);
    local->function_values.reserve(cz::heap_allocator(), 1);
    // TODO: garbage collect / ref count?
    local->function_names.push(key.clone(cz::heap_allocator()));
    local->function_values.push(node);
}

void cleanup_local(Shell_Local* local) {
    for (size_t i = 0; i < local->exported_vars.len; ++i) {
        local->exported_vars[i].drop();
    }
    for (size_t i = 0; i < local->variable_names.len; ++i) {
        local->variable_names[i].drop();
        local->variable_values[i].drop();
    }
    for (size_t i = 0; i < local->unset_vars.len; ++i) {
        local->unset_vars[i].drop();
    }
    for (size_t i = 0; i < local->alias_names.len; ++i) {
        local->alias_names[i].drop(cz::heap_allocator());
    }
    for (size_t i = 0; i < local->function_names.len; ++i) {
        local->function_names[i].drop(cz::heap_allocator());
    }
    for (size_t i = 0; i < local->working_directories.len; ++i) {
        local->working_directories[i].drop(cz::heap_allocator());
    }

    local->exported_vars.drop(cz::heap_allocator());
    local->variable_names.drop(cz::heap_allocator());
    local->variable_values.drop(cz::heap_allocator());
    local->unset_vars.drop(cz::heap_allocator());
    local->alias_names.drop(cz::heap_allocator());
    local->alias_values.drop(cz::heap_allocator());
    local->function_names.drop(cz::heap_allocator());
    local->function_values.drop(cz::heap_allocator());
    // local->args.drop <-- in buffer array, nothing to do
    local->working_directories.drop(cz::heap_allocator());
}
