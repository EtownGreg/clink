// Copyright (c) 2012 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "hook_setter.h"

#include <core/base.h>
#include <core/log.h>
#include <process/hook.h>
#include <process/pe.h>
#include <process/vm.h>

//------------------------------------------------------------------------------
static bool             (*g_hook_trap)()        = nullptr;
static void             (*g_hook_trap_addr)()   = nullptr;
static unsigned char    g_hook_trap_value       = 0;
static void*            g_hook_veh_handle       = nullptr;



//------------------------------------------------------------------------------
static LONG WINAPI hook_trap_veh(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* er;
    void** sp_reg;

    // Check exception record is the exception we've forced.
    er = info->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (er->ExceptionAddress != g_hook_trap_addr)
        return EXCEPTION_CONTINUE_SEARCH;

    // Restore original instruction.
    vm_access().write(
        (void*)g_hook_trap_addr,
        &g_hook_trap_value,
        sizeof(g_hook_trap_value)
    );

    // Who called us?
#if defined(_M_IX86)
    sp_reg = (void**)info->ContextRecord->Esp;
#elif defined(_M_X64)
    sp_reg = (void**)info->ContextRecord->Rsp;
#endif
    LOG("VEH hit - caller is %p.", *sp_reg);

    // Apply hooks.
    if (g_hook_trap != nullptr && !g_hook_trap())
        LOG("Hook trap %p failed.", g_hook_trap);

    RemoveVectoredExceptionHandler(g_hook_veh_handle);
    return EXCEPTION_CONTINUE_EXECUTION;
}

//------------------------------------------------------------------------------
bool set_hook_trap(void* module, const char* func_name, bool (*trap)())
{
    // If there's a debugger attached, we can't use VEH.
    if (IsDebuggerPresent())
        return trap();

    auto* addr = pe_info(module).get_export(func_name);
    if (addr == nullptr)
    {
        char dll[96] = {};
        GetModuleFileName(HMODULE(module), dll, sizeof_array(dll));
        LOG("Unable to resolve address for %s in %s", dll, func_name);
        return false;
    }

    g_hook_trap = trap;
    g_hook_trap_addr = addr;
    g_hook_trap_value = *(unsigned char*)g_hook_trap_addr;
    g_hook_veh_handle = AddVectoredExceptionHandler(1, hook_trap_veh);

    // Write a HALT instruction to force an exception.
    unsigned char to_write = 0xf4;
    vm_access().write((void*)addr, &to_write, sizeof(to_write));

    return true;
}



//------------------------------------------------------------------------------
hook_setter::hook_setter()
: m_desc_count(0)
{
}

//------------------------------------------------------------------------------
bool hook_setter::add_trap(void* module, const char* name, bool (*trap)())
{
    return (add_desc(hook_type_trap, module, name, funcptr_t(trap)) != nullptr);
}

//------------------------------------------------------------------------------
int hook_setter::commit()
{
    // Each hook needs fixing up, so we find the base address of our module.
    void* self = vm_region("clink").get_parent().get_base();
    if (self == nullptr)
        return 0;

    // Apply all the hooks add to the setter.
    int success = 0;
    for (int i = 0; i < m_desc_count; ++i)
    {
        const hook_desc& desc = m_descs[i];
        switch (desc.type)
        {
        case hook_type_iat_by_name: success += !!commit_iat(self, desc);  break;
        case hook_type_jmp:         success += !!commit_jmp(self, desc);  break;
        case hook_type_trap:        success += !!commit_trap(self, desc); break;
        }
    }

    return success;
}

//------------------------------------------------------------------------------
hook_setter::hook_desc* hook_setter::add_desc(
    hook_type type,
    void* module,
    const char* name,
    funcptr_t hook)
{
    if (m_desc_count >= sizeof_array(m_descs))
        return nullptr;

    hook_desc& desc = m_descs[m_desc_count];
    desc.type = type;
    desc.module = module;
    desc.hook = hook;
    desc.name = name;

    ++m_desc_count;
    return &desc;
}

//------------------------------------------------------------------------------
bool hook_setter::commit_iat(void* self, const hook_desc& desc)
{
    funcptr_t addr = hook_iat(desc.module, nullptr, desc.name, desc.hook, 1);
    if (addr == nullptr)
    {
        LOG("Unable to hook %s in IAT at base %p", desc.name, desc.module);
        return false;
    }

    // If the target's IAT was hooked then the hook destination is now
    // stored in 'addr'. We hook ourselves with this address to maintain
    // any IAT hooks that may already exist.
    if (hook_iat(self, nullptr, desc.name, addr, 1) == 0)
    {
        LOG("Failed to hook own IAT for %s", desc.name);
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
bool hook_setter::commit_jmp(void* self, const hook_desc& desc)
{
    // Hook into a DLL's import by patching the start of the function. 'addr' is
    // the trampoline that can be used to call the original. This method doesn't
    // use the IAT.

    auto* addr = hook_jmp(desc.module, desc.name, desc.hook);
    if (addr == nullptr)
    {
        LOG("Unable to hook %s in %p", desc.name, desc.module);
        return false;
    }

    // Patch our own IAT with the address of a trampoline so out use of this
    // function calls the original.
    if (hook_iat(self, nullptr, desc.name, addr, 1) == 0)
    {
        LOG("Failed to hook own IAT for %s", desc.name);
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
bool hook_setter::commit_trap(void* self, const hook_desc& desc)
{
    return set_hook_trap(desc.module, desc.name, (bool (*)())(desc.hook));
}
