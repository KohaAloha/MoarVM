#include "moar.h"
#include <math.h>
#include "platform/time.h"
#include "platform/sys.h"

/* Macros for getting things from the bytecode stream. */
#if MVM_GC_DEBUG >= 2
MVM_STATIC_INLINE MVMuint16 check_reg(MVMThreadContext *tc, MVMRegister *reg_base, MVMuint16 idx) {
    MVMFrame *f = tc->cur_frame;
    MVMuint16 kind = f->spesh_cand && f->spesh_cand->local_types
        ? f->spesh_cand->local_types[idx]
        : f->static_info->body.local_types[idx];
    if (kind == MVM_reg_obj || kind == MVM_reg_str)
        MVM_ASSERT_NOT_FROMSPACE(tc, reg_base[idx].o);
    return idx;
}
/* The bytecode stream is OPs (16 bit numbers) followed by the (16 bit numbers) of the registers
 * the OP needs (return register + argument registers. The pc will point to the first place after
 * the current op, i.e. the first 16 bit register number. We add the requested number to that and
 * use the result as index into the reg_base array which stores the frame's locals. */
#define GET_REG(pc, idx)    reg_base[check_reg(tc, reg_base, *((MVMuint16 *)(pc + idx)))]
#else
#define GET_REG(pc, idx)    reg_base[*((MVMuint16 *)(pc + idx))]
#endif
#if MVM_GC_DEBUG >= 2
MVM_STATIC_INLINE MVMuint16 check_lex(MVMThreadContext *tc, MVMFrame *f, MVMuint16 idx) {
    MVMuint16 kind = f->spesh_cand && f->spesh_cand->lexical_types
        ? f->spesh_cand->lexical_types[idx]
        : f->static_info->body.lexical_types[idx];
    if (kind == MVM_reg_obj || kind == MVM_reg_str)
        MVM_ASSERT_NOT_FROMSPACE(tc, f->env[idx].o);
    return idx;
}
#define GET_LEX(pc, idx, f) f->env[check_lex(tc, f, *((MVMuint16 *)(pc + idx)))]
#else
#define GET_LEX(pc, idx, f) f->env[*((MVMuint16 *)(pc + idx))]
#endif
#define GET_I16(pc, idx)    *((MVMint16 *)(pc + idx))
#define GET_UI16(pc, idx)   *((MVMuint16 *)(pc + idx))
#define GET_I32(pc, idx)    *((MVMint32 *)(pc + idx))
#define GET_UI32(pc, idx)   *((MVMuint32 *)(pc + idx))
#define GET_N32(pc, idx)    *((MVMnum32 *)(pc + idx))

#define NEXT_OP (op = *(MVMuint16 *)(cur_op), cur_op += 2, op)

#if MVM_CGOTO
#define DISPATCH(op)
#define OP(name) OP_ ## name
#define NEXT *LABELS[NEXT_OP]
#else
#define DISPATCH(op) switch (op)
#define OP(name) case MVM_OP_ ## name
#define NEXT runloop
#endif

#define CHECK_CONC(obj) do { if (MVM_UNLIKELY(IS_CONCRETE((MVMObject *)(obj)) == 0)) { error_concreteness(tc, (MVMObject *)(obj), op); } } while (0)

static void error_concreteness(MVMThreadContext *tc, MVMObject *object, MVMuint16 op) {
    MVM_exception_throw_adhoc(tc, "%s requires a concrete object (got a %s type object instead)",
            MVM_op_get_op(op)->name, MVM_6model_get_debug_name(tc, object));
}

static int tracing_enabled = 0;

/* Various spesh ops incorporate a fastcreate, so they can decide to not do
 * the allocation and serve a result from a cache instead. This factors the
 * fastcreate logic out. */
static MVMObject * fastcreate(MVMThreadContext *tc, MVMuint8 *cur_op) {
    /* Assume we're in normal code, so doing a nursery allocation.
     * Also, that there is no initialize. */
    MVMuint16 size       = GET_UI16(cur_op, 2);
    MVMObject *obj       = MVM_gc_allocate_nursery(tc, size);
#if MVM_GC_DEBUG
    if (tc->allocate_in_gen2)
        MVM_panic(1, "Illegal use of a nursery-allocating spesh op when gen2 allocation flag set");
#endif
    obj->st              = (MVMSTable *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 4)];
    obj->header.size     = size;
    obj->header.owner    = tc->thread_id;
    return obj;
}

static MVMuint64 switch_endian(MVMuint64 val, unsigned char size) {
    if (size == 1) {
        return val;
    }
    else if (size == 2) {
        return (MVMuint16)((val & 0x00FF) << 8) | ((val >> 8 ) & 0x00FF);
    }
    else if (size == 4) {
        val = (MVMuint32)((val << 8) & 0xFF00FF00) | ((val >> 8) & 0xFF00FF );
        return (MVMuint32)((val << 16)) | (val >> 16);
    }
    else if (size == 8) {
        val = ((val << 8) & 0xFF00FF00FF00FF00ULL ) | ((val >> 8) & 0x00FF00FF00FF00FFULL );
        val = ((val << 16) & 0xFFFF0000FFFF0000ULL ) | ((val >> 16) & 0x0000FFFF0000FFFFULL );
        return (val << 32) | (val >> 32);
    }

    MVM_panic(1, "Invalid size (%u) when attempting to switch endianness of %"PRIu64"\n", size, val);
}

/* This is the interpreter run loop. We have one of these per thread. */
void MVM_interp_run(MVMThreadContext *tc, void (*initial_invoke)(MVMThreadContext *, void *), void *invoke_data) {
#if MVM_CGOTO
#include "oplabels.h"
#endif

    /* Points to the place in the bytecode right after the current opcode. */
    /* See the NEXT_OP macro for making sense of this */
    MVMuint8 *cur_op = NULL;

    /* The current frame's bytecode start. */
    MVMuint8 *bytecode_start = NULL;

    /* Points to the base of the current register set for the frame we
     * are presently in. */
    MVMRegister *reg_base = NULL;

    /* Points to the current compilation unit. */
    MVMCompUnit *cu = NULL;

    /* The current call site we're constructing. */
    MVMCallsite *cur_callsite = NULL;

    /* Stash addresses of current op, register base and SC deref base
     * in the TC; this will be used by anything that needs to switch
     * the current place we're interpreting. */
    tc->interp_cur_op         = &cur_op;
    tc->interp_bytecode_start = &bytecode_start;
    tc->interp_reg_base       = &reg_base;
    tc->interp_cu             = &cu;

    /* With everything set up, do the initial invocation (exactly what this does
     * varies depending on if this is starting a new thread or is the top-level
     * program entry point). */
    initial_invoke(tc, invoke_data);

    /* initial_invoke is supposed to have setup interpreter state; if it hasn't,
     * it wasn't a 'real' thread. */
    if (!cur_op)
        goto return_label;

    /* Set jump point, for if we arrive back in the interpreter from an
     * exception thrown from C code. */
    setjmp(tc->interp_jump);

    /* Enter runloop. */
    runloop: {
        MVMuint16 op;

#if MVM_TRACING
        if (tracing_enabled) {
            char *trace_line;
            trace_line = MVM_exception_backtrace_line(tc, tc->cur_frame, 0, cur_op);
            fprintf(stderr, "Op %d%s\n", (int)*((MVMuint16 *)cur_op), trace_line);
            /* slow tracing is slow. Feel free to speed it. */
            MVM_free(trace_line);
        }
#endif

        /* The ops should be in the same order here as in the oplist file, so
         * the compiler can can optimise the switch properly. To check if they
         * are in the same order as the oplist use the
         * tools/compare-oplist-interp-order.sh helper script. */
        DISPATCH(NEXT_OP) {
            OP(no_op):
                goto NEXT;
            OP(const_i8):
            OP(const_i16):
            OP(const_i32):
                MVM_exception_throw_adhoc(tc, "const_iX NYI");
            OP(const_i64):
                GET_REG(cur_op, 0).i64 = MVM_BC_get_I64(cur_op, 2);
                cur_op += 10;
                goto NEXT;
            OP(const_n32):
                MVM_exception_throw_adhoc(tc, "const_n32 NYI");
            OP(const_n64):
                GET_REG(cur_op, 0).n64 = MVM_BC_get_N64(cur_op, 2);
                cur_op += 10;
                goto NEXT;
            OP(const_s):
                GET_REG(cur_op, 0).s = MVM_cu_string(tc, cu, GET_UI32(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(set):
                GET_REG(cur_op, 0) = GET_REG(cur_op, 2);
                cur_op += 4;
                goto NEXT;
            OP(extend_u8):
                GET_REG(cur_op, 0).u64 = (MVMuint64)GET_REG(cur_op, 2).u8;
                cur_op += 4;
                goto NEXT;
            OP(extend_u16):
                GET_REG(cur_op, 0).u64 = (MVMuint64)GET_REG(cur_op, 2).u16;
                cur_op += 4;
                goto NEXT;
            OP(extend_u32):
                GET_REG(cur_op, 0).u64 = (MVMuint64)GET_REG(cur_op, 2).u32;
                cur_op += 4;
                goto NEXT;
            OP(extend_i8):
                GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).i8;
                cur_op += 4;
                goto NEXT;
            OP(extend_i16):
                GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).i16;
                cur_op += 4;
                goto NEXT;
            OP(extend_i32):
                GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).i32;
                cur_op += 4;
                goto NEXT;
            OP(trunc_u8):
                GET_REG(cur_op, 0).u8 = (MVMuint8)GET_REG(cur_op, 2).u64;
                cur_op += 4;
                goto NEXT;
            OP(trunc_u16):
                GET_REG(cur_op, 0).u16 = (MVMuint16)GET_REG(cur_op, 2).u64;
                cur_op += 4;
                goto NEXT;
            OP(trunc_u32):
                GET_REG(cur_op, 0).u32 = (MVMuint32)GET_REG(cur_op, 2).u64;
                cur_op += 4;
                goto NEXT;
            OP(trunc_i8):
                GET_REG(cur_op, 0).i8 = (MVMint8)GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(trunc_i16):
                GET_REG(cur_op, 0).i16 = (MVMint16)GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(trunc_i32):
                GET_REG(cur_op, 0).i32 = (MVMint32)GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(extend_n32):
                GET_REG(cur_op, 0).n64 = (MVMnum64)GET_REG(cur_op, 2).n32;
                cur_op += 4;
                goto NEXT;
            OP(trunc_n32):
                GET_REG(cur_op, 0).n32 = (MVMnum32)GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(goto):
                cur_op = bytecode_start + GET_UI32(cur_op, 0);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(if_i):
                if (GET_REG(cur_op, 0).i64)
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                else
                    cur_op += 6;
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(unless_i):
                if (GET_REG(cur_op, 0).i64)
                    cur_op += 6;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(if_n):
                if (GET_REG(cur_op, 0).n64 != 0.0)
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                else
                    cur_op += 6;
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(unless_n):
                if (GET_REG(cur_op, 0).n64 != 0.0)
                    cur_op += 6;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(if_s): {
                MVMString *str = GET_REG(cur_op, 0).s;
                if (!str || MVM_string_graphs(tc, str) == 0)
                    cur_op += 6;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                GC_SYNC_POINT(tc);
                goto NEXT;
            }
            OP(unless_s): {
                MVMString *str = GET_REG(cur_op, 0).s;
                if (!str || MVM_string_graphs(tc, str) == 0)
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                else
                    cur_op += 6;
                GC_SYNC_POINT(tc);
                goto NEXT;
            }
            OP(if_s0): {
                MVMString *str = GET_REG(cur_op, 0).s;
                if (!MVM_coerce_istrue_s(tc, str))
                    cur_op += 6;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                GC_SYNC_POINT(tc);
                goto NEXT;
            }
            OP(unless_s0): {
                MVMString *str = GET_REG(cur_op, 0).s;
                if (!MVM_coerce_istrue_s(tc, str))
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                else
                    cur_op += 6;
                GC_SYNC_POINT(tc);
                goto NEXT;
            }
            OP(if_o):
                GC_SYNC_POINT(tc);
                MVM_coerce_istrue(tc, GET_REG(cur_op, 0).o, NULL,
                    bytecode_start + GET_UI32(cur_op, 2),
                    cur_op + 6,
                    0);
                goto NEXT;
            OP(unless_o):
                GC_SYNC_POINT(tc);
                MVM_coerce_istrue(tc, GET_REG(cur_op, 0).o, NULL,
                    bytecode_start + GET_UI32(cur_op, 2),
                    cur_op + 6,
                    1);
                goto NEXT;
            OP(jumplist): {
                MVMint64 num_labels = MVM_BC_get_I64(cur_op, 0);
                MVMint64 input = GET_REG(cur_op, 8).i64;
                cur_op += 10;
                /* the goto ops are guaranteed valid/existent by validation.c */
                if (input < 0 || input >= num_labels) { /* implicitly covers num_labels == 0 */
                    /* skip the entire goto list block */
                    cur_op += (6 /* size of each goto op */) * num_labels;
                }
                else { /* delve directly into the selected goto op */
                    cur_op = bytecode_start + GET_UI32(cur_op,
                        input * (6 /* size of each goto op */)
                        + (2 /* size of the goto instruction itself */));
                }
                GC_SYNC_POINT(tc);
                goto NEXT;
            }
            OP(getlex): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 idx = GET_UI16(cur_op, 2);
                MVMuint16 outers = GET_UI16(cur_op, 4);
                MVMuint16 *lexical_types;
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                lexical_types = f->spesh_cand && f->spesh_cand->lexical_types
                    ? f->spesh_cand->lexical_types
                    : f->static_info->body.lexical_types;
                if (lexical_types[idx] == MVM_reg_obj) {
                    MVMRegister found = GET_LEX(cur_op, 2, f);
                    MVMObject *value = found.o == NULL
                        ? MVM_frame_vivify_lexical(tc, f, idx)
                        : found.o;
                    GET_REG(cur_op, 0).o = value;
                    if (MVM_spesh_log_is_logging(tc))
                        MVM_spesh_log_type(tc, value);
                }
                else {
                    GET_REG(cur_op, 0) = GET_LEX(cur_op, 2, f);
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(bindlex): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 outers = GET_UI16(cur_op, 2);
                MVMuint16 kind = f->spesh_cand && f->spesh_cand->local_types
                    ? f->spesh_cand->local_types[GET_UI16(cur_op, 4)]
                    : f->static_info->body.local_types[GET_UI16(cur_op, 4)];
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "bindlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                if (kind == MVM_reg_obj || kind == MVM_reg_str) {
#if MVM_GC_DEBUG
                    MVM_ASSERT_NOT_FROMSPACE(tc, GET_REG(cur_op, 4).o);
#endif
                    MVM_ASSIGN_REF(tc, &(f->header), GET_LEX(cur_op, 0, f).o,
                        GET_REG(cur_op, 4).o);
                }
                else {
                    GET_LEX(cur_op, 0, f) = GET_REG(cur_op, 4);
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(getlex_ni):
                GET_REG(cur_op, 0).i64 = MVM_frame_find_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_reg_int64)->i64;
                cur_op += 6;
                goto NEXT;
            OP(getlex_nn):
                GET_REG(cur_op, 0).n64 = MVM_frame_find_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_reg_num64)->n64;
                cur_op += 6;
                goto NEXT;
            OP(getlex_ns):
                GET_REG(cur_op, 0).s = MVM_frame_find_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_reg_str)->s;
                cur_op += 6;
                goto NEXT;
            OP(getlex_no): {
                MVMRegister *found = MVM_frame_find_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_reg_obj);
                if (found) {
                    GET_REG(cur_op, 0).o = found->o;
                    if (MVM_spesh_log_is_logging(tc))
                        MVM_spesh_log_type(tc, found->o);
                }
                else {
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(bindlex_ni):
                MVM_frame_bind_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 0)),
                    MVM_reg_int64, GET_REG(cur_op, 4));
                cur_op += 6;
                goto NEXT;
            OP(bindlex_nn):
                MVM_frame_bind_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 0)),
                    MVM_reg_num64, GET_REG(cur_op, 4));
                cur_op += 6;
                goto NEXT;
            OP(bindlex_ns):
                MVM_frame_bind_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 0)),
                    MVM_reg_str, GET_REG(cur_op, 4));
                cur_op += 6;
                goto NEXT;
            OP(bindlex_no):
                MVM_frame_bind_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 0)),
                    MVM_reg_obj, GET_REG(cur_op, 4));
                cur_op += 6;
                goto NEXT;
            OP(getlex_ng):
            OP(bindlex_ng):
                MVM_exception_throw_adhoc(tc, "get/bindlex_ng NYI");
            OP(getdynlex): {
                if (MVM_UNLIKELY(tc->cur_frame->caller == 0)) {
                    MVM_exception_throw_adhoc(tc, "cannot call getdynlex without a caller frame");
                }
                GET_REG(cur_op, 0).o = MVM_frame_getdynlex(tc, GET_REG(cur_op, 2).s,
                        tc->cur_frame->caller);
                cur_op += 4;
                goto NEXT;
            }
            OP(binddynlex): {
                MVM_frame_binddynlex(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).o,
                        tc->cur_frame->caller);
                cur_op += 4;
                goto NEXT;
            }
            OP(setlexvalue): {
                MVMObject *code = GET_REG(cur_op, 0).o;
                MVMString *name = MVM_cu_string(tc, cu, GET_UI32(cur_op, 2));
                MVMObject *val  = GET_REG(cur_op, 6).o;
                MVMint16   flag = GET_I16(cur_op, 8);
                if (flag < 0 || flag > 2)
                    MVM_exception_throw_adhoc(tc, "setlexvalue provided with invalid flag");
                if (IS_CONCRETE(code) && REPR(code)->ID == MVM_REPR_ID_MVMCode) {
                    MVMStaticFrame *sf = ((MVMCode *)code)->body.sf;
                    MVMuint8 found = 0;
                    if (!sf->body.fully_deserialized)
                        MVM_bytecode_finish_frame(tc, sf->body.cu, sf, 0);
                    if (sf->body.lexical_names) {
                        MVMLexicalRegistry *entry;
                        MVM_HASH_GET(tc, sf->body.lexical_names, name, entry);
                        if (entry && sf->body.lexical_types[entry->value] == MVM_reg_obj) {
                            MVM_ASSIGN_REF(tc, &(sf->common.header), sf->body.static_env[entry->value].o, val);
                            sf->body.static_env_flags[entry->value] = (MVMuint8)flag;
                            found = 1;
                        }
                    }
                    if (!found)
                        MVM_exception_throw_adhoc(tc, "setstaticlex given invalid lexical name");
                }
                else {
                    MVM_exception_throw_adhoc(tc, "setstaticlex needs a code ref");
                }
                cur_op += 10;
                goto NEXT;
            }
            OP(lexprimspec): {
                MVMObject *ctx  = GET_REG(cur_op, 2).o;
                MVMString *name = GET_REG(cur_op, 4).s;
                if (REPR(ctx)->ID != MVM_REPR_ID_MVMContext || !IS_CONCRETE(ctx))
                    MVM_exception_throw_adhoc(tc, "lexprimspec needs a context");
                GET_REG(cur_op, 0).i64 = MVM_context_lexical_primspec(tc,
                    (MVMContext *)ctx, name);
                cur_op += 6;
                goto NEXT;
            }
            OP(return_i):
                MVM_args_set_result_int(tc, GET_REG(cur_op, 0).i64,
                    MVM_RETURN_CALLER_FRAME);
                if (MVM_frame_try_return(tc) == 0)
                    goto return_label;
                goto NEXT;
            OP(return_n):
                MVM_args_set_result_num(tc, GET_REG(cur_op, 0).n64,
                    MVM_RETURN_CALLER_FRAME);
                if (MVM_frame_try_return(tc) == 0)
                    goto return_label;
                goto NEXT;
            OP(return_s):
                MVM_args_set_result_str(tc, GET_REG(cur_op, 0).s,
                    MVM_RETURN_CALLER_FRAME);
                if (MVM_frame_try_return(tc) == 0)
                    goto return_label;
                goto NEXT;
            OP(return_o): {
                MVMObject *value = GET_REG(cur_op, 0).o;
                MVM_args_set_result_obj(tc, value, MVM_RETURN_CALLER_FRAME);
                if (MVM_frame_try_return(tc) == 0)
                    goto return_label;
                goto NEXT;
            }
            OP(return):
                MVM_args_assert_void_return_ok(tc, MVM_RETURN_CALLER_FRAME);
                if (MVM_frame_try_return(tc) == 0)
                    goto return_label;
                goto NEXT;
            OP(eq_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 == GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(ne_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 != GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(lt_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 <  GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(le_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 <= GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(gt_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >  GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(ge_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >= GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(cmp_i): {
                MVMint64 a = GET_REG(cur_op, 2).i64, b = GET_REG(cur_op, 4).i64;
                GET_REG(cur_op, 0).i64 = (a > b) - (a < b);
                cur_op += 6;
                goto NEXT;
            }
            OP(add_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 + GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(sub_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 - GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(mul_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 * GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(div_i): {
                MVMint64 num   = GET_REG(cur_op, 2).i64;
                MVMint64 denom = GET_REG(cur_op, 4).i64;
                /* if we have a negative result, make sure we floor rather
                 * than rounding towards zero. */
                if (denom == 0)
                    MVM_exception_throw_adhoc(tc, "Division by zero");
                if ((num < 0) ^ (denom < 0)) {
                    if ((num % denom) != 0) {
                        GET_REG(cur_op, 0).i64 = num / denom - 1;
                    } else {
                        GET_REG(cur_op, 0).i64 = num / denom;
                    }
                } else {
                    GET_REG(cur_op, 0).i64 = num / denom;
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(div_u):
                GET_REG(cur_op, 0).u64 = GET_REG(cur_op, 2).u64 / GET_REG(cur_op, 4).u64;
                cur_op += 6;
                goto NEXT;
            OP(mod_i): {
                MVMint64 numer = GET_REG(cur_op, 2).i64;
                MVMint64 denom = GET_REG(cur_op, 4).i64;
                if (denom == 0)
                    MVM_exception_throw_adhoc(tc, "Modulation by zero");
                GET_REG(cur_op, 0).i64 = numer % denom;
                cur_op += 6;
                goto NEXT;
            }
            OP(mod_u):
                GET_REG(cur_op, 0).u64 = GET_REG(cur_op, 2).u64 % GET_REG(cur_op, 4).u64;
                cur_op += 6;
                goto NEXT;
            OP(neg_i):
                GET_REG(cur_op, 0).i64 = -GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(abs_i): {
                MVMint64 v    = GET_REG(cur_op, 2).i64;
                MVMint64 mask = v >> 63;
                GET_REG(cur_op, 0).i64 = (v + mask) ^ mask;
                cur_op += 4;
                goto NEXT;
            }
            OP(inc_i):
                GET_REG(cur_op, 0).i64++;
                cur_op += 2;
                goto NEXT;
            OP(inc_u):
                GET_REG(cur_op, 0).u64++;
                cur_op += 2;
                goto NEXT;
            OP(dec_i):
                GET_REG(cur_op, 0).i64--;
                cur_op += 2;
                goto NEXT;
            OP(dec_u):
                GET_REG(cur_op, 0).u64--;
                cur_op += 2;
                goto NEXT;
            OP(band_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 & GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(bor_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 | GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(bxor_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 ^ GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(bnot_i):
                GET_REG(cur_op, 0).i64 = ~GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(blshift_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 << GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(brshift_i):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >> GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            OP(pow_i): {
                    MVMint64 base = GET_REG(cur_op, 2).i64;
                    MVMint64 exp = GET_REG(cur_op, 4).i64;
                    MVMint64 result = 1;
                    /* "Exponentiation by squaring" */
                    if (exp < 0) {
                        result = 0; /* because 1/base**-exp is between 0 and 1 */
                    }
                    else {
                        while (exp) {
                            if (exp & 1)
                                result *= base;
                            exp >>= 1;
                            base *= base;
                        }
                    }
                    GET_REG(cur_op, 0).i64 = result;
                }
                cur_op += 6;
                goto NEXT;
            OP(not_i): {
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 ? 0 : 1;
                cur_op += 4;
                goto NEXT;
            }
            OP(gcd_i): {
                MVMint64 a = labs(GET_REG(cur_op, 2).i64), b = labs(GET_REG(cur_op, 4).i64), c;
                while ( b != 0 ) {
                    c = a % b; a = b; b = c;
                }
                GET_REG(cur_op, 0).i64 = a;
                cur_op += 6;
                goto NEXT;
            }
            OP(lcm_i): {
                MVMint64 a = GET_REG(cur_op, 2).i64, b = GET_REG(cur_op, 4).i64, c, a_ = a, b_ = b;
                while ( b != 0 ) {
                    c = a % b; a = b; b = c;
                }
                c = a;
                GET_REG(cur_op, 0).i64 = a_ / c * b_;
                cur_op += 6;
                goto NEXT;
            }
            OP(eq_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 == GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(ne_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 != GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(lt_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 <  GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(le_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 <= GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(gt_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 >  GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(ge_n):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 >= GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(cmp_n): {
                MVMnum64 a = GET_REG(cur_op, 2).n64, b = GET_REG(cur_op, 4).n64;
                GET_REG(cur_op, 0).i64 = (a > b) - (a < b);
                cur_op += 6;
                goto NEXT;
            }
            OP(add_n):
                GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 + GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(sub_n):
                GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 - GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(mul_n):
                GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 * GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(div_n):
                GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 / GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            OP(mod_n): {
                MVMnum64 a = GET_REG(cur_op, 2).n64;
                MVMnum64 b = GET_REG(cur_op, 4).n64;
                GET_REG(cur_op, 0).n64 = b == 0 ? a : a - b * floor(a / b);
                cur_op += 6;
                goto NEXT;
            }
            OP(neg_n):
                GET_REG(cur_op, 0).n64 = -GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(abs_n):
                GET_REG(cur_op, 0).n64 = fabs(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(pow_n):
                GET_REG(cur_op, 0).n64 = pow(GET_REG(cur_op, 2).n64, GET_REG(cur_op, 4).n64);
                cur_op += 6;
                goto NEXT;
            OP(ceil_n):
                GET_REG(cur_op, 0).n64 = ceil(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(floor_n):
                GET_REG(cur_op, 0).n64 = floor(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(sin_n):
                GET_REG(cur_op, 0).n64 = sin(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(asin_n):
                GET_REG(cur_op, 0).n64 = asin(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(cos_n):
                GET_REG(cur_op, 0).n64 = cos(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(acos_n):
                GET_REG(cur_op, 0).n64 = acos(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(tan_n):
                GET_REG(cur_op, 0).n64 = tan(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(atan_n):
                GET_REG(cur_op, 0).n64 = atan(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(atan2_n):
                GET_REG(cur_op, 0).n64 = atan2(GET_REG(cur_op, 2).n64,
                    GET_REG(cur_op, 4).n64);
                cur_op += 6;
                goto NEXT;
            OP(sec_n): /* XXX TODO) handle edge cases */
                GET_REG(cur_op, 0).n64 = 1.0 / cos(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(asec_n): /* XXX TODO) handle edge cases */
                GET_REG(cur_op, 0).n64 = acos(1.0 / GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(sinh_n):
                GET_REG(cur_op, 0).n64 = sinh(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(cosh_n):
                GET_REG(cur_op, 0).n64 = cosh(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(tanh_n):
                GET_REG(cur_op, 0).n64 = tanh(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(sech_n): /* XXX TODO) handle edge cases */
                GET_REG(cur_op, 0).n64 = 1.0 / cosh(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(sqrt_n):
                GET_REG(cur_op, 0).n64 = sqrt(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(log_n):
                GET_REG(cur_op, 0).n64 = log(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(exp_n):
                GET_REG(cur_op, 0).n64 = exp(GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(coerce_in):
                GET_REG(cur_op, 0).n64 = (MVMnum64)GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(coerce_ni):
                GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(coerce_is):
                GET_REG(cur_op, 0).s = MVM_coerce_i_s(tc, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(coerce_ns):
                GET_REG(cur_op, 0).s = MVM_coerce_n_s(tc, GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(coerce_si):
                GET_REG(cur_op, 0).i64 = MVM_coerce_s_i(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(coerce_sn):
                GET_REG(cur_op, 0).n64 = MVM_coerce_s_n(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(smrt_intify): {
                /* Increment PC before calling coercer, as it may make
                 * a method call to get the result. */
                MVMObject   *obj = GET_REG(cur_op, 2).o;
                MVMRegister *res = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_coerce_smart_intify(tc, obj, res);
                goto NEXT;
            }
            OP(smrt_numify): {
                /* Increment PC before calling coercer, as it may make
                 * a method call to get the result. */
                MVMObject   *obj = GET_REG(cur_op, 2).o;
                MVMRegister *res = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_coerce_smart_numify(tc, obj, res);
                goto NEXT;
            }
            OP(smrt_strify): {
                /* Increment PC before calling coercer, as it may make
                 * a method call to get the result. */
                MVMObject   *obj = GET_REG(cur_op, 2).o;
                MVMRegister *res = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_coerce_smart_stringify(tc, obj, res);
                goto NEXT;
            }
            OP(prepargs):
                /* Store callsite in the frame so that the GC knows how to mark
                 * any arguments. Note that since none of the arg-setting ops can
                 * trigger GC, there's no way the setup can be interrupted, so we
                 * don't need to clear the args buffer before we start. */
                cur_callsite = cu->body.callsites[GET_UI16(cur_op, 0)];
                tc->cur_frame->cur_args_callsite = cur_callsite;
                cur_op += 2;
                goto NEXT;
            OP(arg_i):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].i64 = GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(arg_n):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].n64 = GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(arg_s):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].s = GET_REG(cur_op, 2).s;
                cur_op += 4;
                goto NEXT;
            OP(arg_o):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].o = GET_REG(cur_op, 2).o;
                cur_op += 4;
                goto NEXT;
            OP(argconst_i):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].i64 = MVM_BC_get_I64(cur_op, 2);
                cur_op += 10;
                goto NEXT;
            OP(argconst_n):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].n64 = MVM_BC_get_N64(cur_op, 2);
                cur_op += 10;
                goto NEXT;
            OP(argconst_s):
                tc->cur_frame->args[GET_UI16(cur_op, 0)].s =
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(invoke_v):
                {
                    MVMObject   *code = GET_REG(cur_op, 0).o;
                    MVMRegister *args = tc->cur_frame->args;
                    MVMuint16 was_multi = 0;
                    /* was_multi argument is MVMuint16* */
                    code = MVM_frame_find_invokee_multi_ok(tc, code, &cur_callsite, args,
                        &was_multi);
                    if (MVM_spesh_log_is_logging(tc)) {
                        MVMROOT(tc, code, {
                            /* was_multi is MVMint16 */
                            MVM_spesh_log_invoke_target(tc, code, was_multi);
                        });
                    }
                    tc->cur_frame->return_value = NULL;
                    tc->cur_frame->return_type = MVM_RETURN_VOID;
                    cur_op += 2;
                    tc->cur_frame->return_address = cur_op;
                    STABLE(code)->invoke(tc, code, cur_callsite, args);
                }
                goto NEXT;
            OP(invoke_i):
                {
                    MVMObject   *code = GET_REG(cur_op, 2).o;
                    MVMRegister *args = tc->cur_frame->args;
                    MVMuint16 was_multi = 0;
                    code = MVM_frame_find_invokee_multi_ok(tc, code, &cur_callsite, args,
                        &was_multi);
                    if (MVM_spesh_log_is_logging(tc)) {
                        MVMROOT(tc, code, {
                            MVM_spesh_log_invoke_target(tc, code, was_multi);
                        });
                    }
                    tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                    tc->cur_frame->return_type = MVM_RETURN_INT;
                    cur_op += 4;
                    tc->cur_frame->return_address = cur_op;
                    STABLE(code)->invoke(tc, code, cur_callsite, args);
                }
                goto NEXT;
            OP(invoke_n):
                {
                    MVMObject   *code = GET_REG(cur_op, 2).o;
                    MVMRegister *args = tc->cur_frame->args;
                    MVMuint16 was_multi = 0;
                    code = MVM_frame_find_invokee_multi_ok(tc, code, &cur_callsite, args,
                        &was_multi);
                    if (MVM_spesh_log_is_logging(tc)) {
                        MVMROOT(tc, code, {
                            MVM_spesh_log_invoke_target(tc, code, was_multi);
                        });
                    }
                    tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                    tc->cur_frame->return_type = MVM_RETURN_NUM;
                    cur_op += 4;
                    tc->cur_frame->return_address = cur_op;
                    STABLE(code)->invoke(tc, code, cur_callsite, args);
                }
                goto NEXT;
            OP(invoke_s):
                {
                    MVMObject   *code = GET_REG(cur_op, 2).o;
                    MVMRegister *args = tc->cur_frame->args;
                    MVMuint16 was_multi = 0;
                    code = MVM_frame_find_invokee_multi_ok(tc, code, &cur_callsite, args,
                        &was_multi);
                    if (MVM_spesh_log_is_logging(tc)) {
                        MVMROOT(tc, code, {
                            MVM_spesh_log_invoke_target(tc, code, was_multi);
                        });
                    }
                    tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                    tc->cur_frame->return_type = MVM_RETURN_STR;
                    cur_op += 4;
                    tc->cur_frame->return_address = cur_op;
                    STABLE(code)->invoke(tc, code, cur_callsite, args);
                }
                goto NEXT;
            OP(invoke_o):
                {
                    MVMObject   *code = GET_REG(cur_op, 2).o;
                    MVMRegister *args = tc->cur_frame->args;
                    MVMuint16 was_multi = 0;
                    code = MVM_frame_find_invokee_multi_ok(tc, code, &cur_callsite, args,
                        &was_multi);
                    if (MVM_spesh_log_is_logging(tc)) {
                        MVMROOT(tc, code, {
                            MVM_spesh_log_invoke_target(tc, code, was_multi);
                        });
                    }
                    tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                    tc->cur_frame->return_type = MVM_RETURN_OBJ;
                    cur_op += 4;
                    tc->cur_frame->return_address = cur_op;
                    STABLE(code)->invoke(tc, code, cur_callsite, args);
                }
                goto NEXT;
            OP(checkarity):
                MVM_args_checkarity(tc, &tc->cur_frame->params, GET_UI16(cur_op, 0), GET_UI16(cur_op, 2));
                cur_op += 4;
                goto NEXT;
            OP(param_rp_i):
                GET_REG(cur_op, 0).i64 = MVM_args_get_required_pos_int(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                cur_op += 4;
                goto NEXT;
            OP(param_rp_n):
                GET_REG(cur_op, 0).n64 = MVM_args_get_required_pos_num(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2)).arg.n64;
                cur_op += 4;
                goto NEXT;
            OP(param_rp_s):
                GET_REG(cur_op, 0).s = MVM_args_get_required_pos_str(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                cur_op += 4;
                goto NEXT;
            OP(param_rp_o): {
                MVMuint16 arg_idx = GET_UI16(cur_op, 2);
                MVMObject *param = MVM_args_get_required_pos_obj(tc, &tc->cur_frame->params, arg_idx);
                GET_REG(cur_op, 0).o = param;
                cur_op += 4;
                goto NEXT;
            }
            OP(param_op_i):
            {
                MVMArgInfo param = MVM_args_get_optional_pos_int(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                if (param.exists) {
                    GET_REG(cur_op, 0).i64 = param.arg.i64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 4);
                }
                else {
                    cur_op += 8;
                }
                goto NEXT;
            }
            OP(param_op_n):
            {
                MVMArgInfo param = MVM_args_get_optional_pos_num(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                if (param.exists) {
                    GET_REG(cur_op, 0).n64 = param.arg.n64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 4);
                }
                else {
                    cur_op += 8;
                }
                goto NEXT;
            }
            OP(param_op_s):
            {
                MVMArgInfo param = MVM_args_get_optional_pos_str(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                if (param.exists) {
                    GET_REG(cur_op, 0).s = param.arg.s;
                    cur_op = bytecode_start + GET_UI32(cur_op, 4);
                }
                else {
                    cur_op += 8;
                }
                goto NEXT;
            }
            OP(param_op_o):
            {
                MVMuint16 arg_idx = GET_UI16(cur_op, 2);
                MVMArgInfo param = MVM_args_get_optional_pos_obj(tc, &tc->cur_frame->params, arg_idx);
                if (param.exists) {
                    GET_REG(cur_op, 0).o = param.arg.o;
                    cur_op = bytecode_start + GET_UI32(cur_op, 4);
                }
                else {
                    cur_op += 8;
                }
                goto NEXT;
            }
            OP(param_rn_i):
                GET_REG(cur_op, 0).i64 = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_REQUIRED).arg.i64;
                cur_op += 6;
                goto NEXT;
            OP(param_rn_n):
                GET_REG(cur_op, 0).n64 = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_REQUIRED).arg.n64;
                cur_op += 6;
                goto NEXT;
            OP(param_rn_s):
                GET_REG(cur_op, 0).s = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_REQUIRED).arg.s;
                cur_op += 6;
                goto NEXT;
            OP(param_rn_o): {
                MVMArgInfo param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_REQUIRED);
                GET_REG(cur_op, 0).o = param.arg.o;
                cur_op += 6;
                goto NEXT;
            }
            OP(param_on_i):
            {
                MVMArgInfo param = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).i64 = param.arg.i64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 6);
                }
                else {
                    cur_op += 10;
                }
                goto NEXT;
            }
            OP(param_on_n):
            {
                MVMArgInfo param = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).n64 = param.arg.n64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 6);
                }
                else {
                    cur_op += 10;
                }
                goto NEXT;
            }
            OP(param_on_s):
            {
                MVMArgInfo param = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).s = param.arg.s;
                    cur_op = bytecode_start + GET_UI32(cur_op, 6);
                }
                else {
                    cur_op += 10;
                }
                goto NEXT;
            }
            OP(param_on_o):
            {
                MVMArgInfo param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).o = param.arg.o;
                    cur_op = bytecode_start + GET_UI32(cur_op, 6);
                }
                else {
                    cur_op += 10;
                }
                goto NEXT;
            }
            OP(param_sp):
                GET_REG(cur_op, 0).o = MVM_args_slurpy_positional(tc, &tc->cur_frame->params, GET_UI16(cur_op, 2));
                cur_op += 4;
                goto NEXT;
            OP(param_sn):
                GET_REG(cur_op, 0).o = MVM_args_slurpy_named(tc, NULL);
                cur_op += 2;
                goto NEXT;
            OP(getcode):
                GET_REG(cur_op, 0).o = cu->body.coderefs[GET_UI16(cur_op, 2)];
                cur_op += 4;
                goto NEXT;
            OP(caller): {
                MVMFrame *caller = tc->cur_frame;
                MVMint64 depth = GET_REG(cur_op, 2).i64;

                while (caller && depth-- > 0) /* keep the > 0. */
                    caller = caller->caller;

                GET_REG(cur_op, 0).o = caller ? caller->code_ref : tc->instance->VMNull;

                cur_op += 4;
                goto NEXT;
            }
            OP(capturelex):
                MVM_frame_capturelex(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(takeclosure):
                GET_REG(cur_op, 0).o = MVM_frame_takeclosure(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(exception):
                GET_REG(cur_op, 0).o = tc->active_handlers
                    ? tc->active_handlers->ex_obj
                    : tc->instance->VMNull;
                cur_op += 2;
                goto NEXT;
            OP(bindexmessage): {
                MVMObject *ex = GET_REG(cur_op, 0).o;
                if (IS_CONCRETE(ex) && REPR(ex)->ID == MVM_REPR_ID_MVMException) {
                    MVM_ASSIGN_REF(tc, &(ex->header), ((MVMException *)ex)->body.message,
                        GET_REG(cur_op, 2).s);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "bindexmessage needs a VMException, got %s (%s)", REPR(ex)->name, MVM_6model_get_debug_name(tc, ex));
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(bindexpayload): {
                MVMObject *ex = GET_REG(cur_op, 0).o;
                MVM_bind_exception_payload(tc, ex, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(bindexcategory): {
                MVMObject *ex = GET_REG(cur_op, 0).o;
                MVM_bind_exception_category(tc, ex, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            }
            OP(getexmessage): {
                MVMObject *ex = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(ex) && REPR(ex)->ID == MVM_REPR_ID_MVMException)
                    GET_REG(cur_op, 0).s = ((MVMException *)ex)->body.message;
                else
                    MVM_exception_throw_adhoc(tc, "getexmessage needs a VMException, got %s (%s)", REPR(ex)->name, MVM_6model_get_debug_name(tc, ex));
                cur_op += 4;
                goto NEXT;
            }
            OP(getexpayload): {
                MVMObject *ex = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).o = MVM_get_exception_payload(tc, ex);
                cur_op += 4;
                goto NEXT;
            }
            OP(getexcategory): {
                GET_REG(cur_op, 0).i64 = MVM_get_exception_category(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(throwdyn): {
                MVMRegister *rr     = &GET_REG(cur_op, 0);
                MVMObject   *ex_obj = GET_REG(cur_op, 2).o;
                cur_op += 4;
                MVM_exception_throwobj(tc, MVM_EX_THROW_DYN, ex_obj, rr);
                goto NEXT;
            }
            OP(throwlex): {
                MVMRegister *rr     = &GET_REG(cur_op, 0);
                MVMObject   *ex_obj = GET_REG(cur_op, 2).o;
                cur_op += 4;
                MVM_exception_throwobj(tc, MVM_EX_THROW_LEX, ex_obj, rr);
                goto NEXT;
            }
            OP(throwlexotic): {
                MVMRegister *rr     = &GET_REG(cur_op, 0);
                MVMObject   *ex_obj = GET_REG(cur_op, 2).o;
                cur_op += 4;
                MVM_exception_throwobj(tc, MVM_EX_THROW_LEXOTIC, ex_obj, rr);
                goto NEXT;
            }
            OP(throwcatdyn): {
                MVMRegister *rr  = &GET_REG(cur_op, 0);
                MVMuint32    cat = (MVMuint32)MVM_BC_get_I64(cur_op, 2);
                cur_op += 4;
                MVM_exception_throwcat(tc, MVM_EX_THROW_DYN, cat, rr);
                goto NEXT;
            }
            OP(throwcatlex): {
                MVMRegister *rr  = &GET_REG(cur_op, 0);
                MVMuint32    cat = (MVMuint32)MVM_BC_get_I64(cur_op, 2);
                cur_op += 4;
                MVM_exception_throwcat(tc, MVM_EX_THROW_LEX, cat, rr);
                goto NEXT;
            }
            OP(throwcatlexotic): {
                MVMRegister *rr  = &GET_REG(cur_op, 0);
                MVMuint32    cat = (MVMuint32)MVM_BC_get_I64(cur_op, 2);
                cur_op += 4;
                MVM_exception_throwcat(tc, MVM_EX_THROW_LEXOTIC, cat, rr);
                goto NEXT;
            }
            OP(die): {
                MVMRegister  *rr = &GET_REG(cur_op, 0);
                MVMString   *str =  GET_REG(cur_op, 2).s;
                cur_op += 4;
                MVM_exception_die(tc, str, rr);
                goto NEXT;
            }
            OP(rethrow): {
                MVM_exception_throwobj(tc, MVM_EX_THROW_DYN, GET_REG(cur_op, 0).o, NULL);
                goto NEXT;
            }
            OP(resume):
                /* Expect that resume will set the PC, so don't update cur_op
                 * here. */
                MVM_exception_resume(tc, GET_REG(cur_op, 0).o);
                goto NEXT;
            OP(takehandlerresult): {
                GET_REG(cur_op, 0).o = tc->last_handler_result
                    ? tc->last_handler_result
                    : tc->instance->VMNull;
                tc->last_handler_result = NULL;
                cur_op += 2;
                goto NEXT;
            }
            OP(backtracestrings):
                GET_REG(cur_op, 0).o = MVM_exception_backtrace_strings(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(usecapture):
                GET_REG(cur_op, 0).o = MVM_args_use_capture(tc, tc->cur_frame);
                cur_op += 2;
                goto NEXT;
            OP(savecapture): {
                /* Create a new call capture object. */
                GET_REG(cur_op, 0).o = MVM_args_save_capture(tc, tc->cur_frame);
                cur_op += 2;
                goto NEXT;
            }
            OP(captureposelems): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).i64 = cc->body.apc->num_pos;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposelems needs a MVMCallCapture");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(captureposarg): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).o = MVM_args_get_required_pos_obj(tc, cc->body.apc,
                        (MVMuint32)GET_REG(cur_op, 4).i64);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposarg needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(captureposarg_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).i64 = MVM_args_get_required_pos_int(tc, cc->body.apc,
                        (MVMuint32)GET_REG(cur_op, 4).i64);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposarg_i needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(captureposarg_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).n64 = MVM_args_get_required_pos_num(tc, cc->body.apc,
                        (MVMuint32)GET_REG(cur_op, 4).i64).arg.n64;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposarg_n needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(captureposarg_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).s = MVM_args_get_required_pos_str(tc, cc->body.apc,
                        (MVMuint32)GET_REG(cur_op, 4).i64);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposarg_s needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(captureposprimspec): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMint64   i   = GET_REG(cur_op, 4).i64;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    if (i >= 0 && i < cc->body.apc->num_pos) {
                        MVMCallsiteEntry *arg_flags = cc->body.apc->arg_flags
                            ? cc->body.apc->arg_flags
                            : cc->body.apc->callsite->arg_flags;
                        switch (arg_flags[i] & MVM_CALLSITE_ARG_MASK) {
                            case MVM_CALLSITE_ARG_INT:
                                GET_REG(cur_op, 0).i64 = MVM_STORAGE_SPEC_BP_INT;
                                break;
                            case MVM_CALLSITE_ARG_NUM:
                                GET_REG(cur_op, 0).i64 = MVM_STORAGE_SPEC_BP_NUM;
                                break;
                            case MVM_CALLSITE_ARG_STR:
                                GET_REG(cur_op, 0).i64 = MVM_STORAGE_SPEC_BP_STR;
                                break;
                            default:
                                GET_REG(cur_op, 0).i64 = MVM_STORAGE_SPEC_BP_NONE;
                                break;
                        }
                    }
                    else {
                        MVM_exception_throw_adhoc(tc,
                            "Bad argument index given to captureposprimspec");
                    }
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureposprimspec needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(captureexistsnamed): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).i64 = MVM_args_has_named(tc, cc->body.apc,
                        GET_REG(cur_op, 4).s);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "captureexistsnamed needs a MVMCallCapture");
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(capturehasnameds): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    /* If positionals count doesn't match arg count, we must
                     * have some named args. */
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).i64 = cc->body.apc->arg_count != cc->body.apc->num_pos;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "capturehasnameds needs a MVMCallCapture");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(invokewithcapture): {
                MVMObject *cobj = GET_REG(cur_op, 4).o;
                if (IS_CONCRETE(cobj) && REPR(cobj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMObject *code = GET_REG(cur_op, 2).o;
                    MVMCallCapture *cc = (MVMCallCapture *)cobj;
                    MVMFrameExtra *e = MVM_frame_extra(tc, tc->cur_frame);
                    code = MVM_frame_find_invokee(tc, code, NULL);
                    tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                    tc->cur_frame->return_type = MVM_RETURN_OBJ;
                    cur_op += 6;
                    tc->cur_frame->return_address = cur_op;
                    MVMROOT(tc, cobj, {
                        STABLE(code)->invoke(tc, code, cc->body.apc->callsite,
                            cc->body.apc->args);
                    });
                    if (MVM_FRAME_IS_ON_CALLSTACK(tc, tc->cur_frame)) {
                        e->invoked_call_capture = cobj;
                    }
                    else {
                        MVM_ASSIGN_REF(tc, &(tc->cur_frame->header),
                            e->invoked_call_capture, cobj);
                    }
                    goto NEXT;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "invokewithcapture needs a MVMCallCapture");
                }
            }
            OP(multicacheadd):
                GET_REG(cur_op, 0).o = MVM_multi_cache_add(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            OP(multicachefind):
                GET_REG(cur_op, 0).o = MVM_multi_cache_find(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(null_s):
                GET_REG(cur_op, 0).s = NULL;
                cur_op += 2;
                goto NEXT;
            OP(isnull_s):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).s ? 0 : 1;
                cur_op += 4;
                goto NEXT;
            OP(eq_s):
                GET_REG(cur_op, 0).i64 = MVM_string_equal(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(ne_s):
                GET_REG(cur_op, 0).i64 = (MVMint64)(MVM_string_equal(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s)? 0 : 1);
                cur_op += 6;
                goto NEXT;
            OP(gt_s):
                GET_REG(cur_op, 0).i64 = MVM_string_compare(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s) == 1;
                cur_op += 6;
                goto NEXT;
            OP(ge_s):
                GET_REG(cur_op, 0).i64 = MVM_string_compare(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s) >= 0;
                cur_op += 6;
                goto NEXT;
            OP(lt_s):
                GET_REG(cur_op, 0).i64 = MVM_string_compare(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s) == -1;
                cur_op += 6;
                goto NEXT;
            OP(le_s):
                GET_REG(cur_op, 0).i64 = MVM_string_compare(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s) <= 0;
                cur_op += 6;
                goto NEXT;
            OP(cmp_s):
                GET_REG(cur_op, 0).i64 = MVM_string_compare(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(eqat_s):
                GET_REG(cur_op, 0).i64 = MVM_string_equal_at(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(eqatic_s):
                GET_REG(cur_op, 0).i64 = MVM_string_equal_at_ignore_case(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(haveat_s):
                GET_REG(cur_op, 0).i64 = MVM_string_have_at(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).s,
                    GET_REG(cur_op, 10).i64);
                cur_op += 12;
                goto NEXT;
            OP(concat_s):
                GET_REG(cur_op, 0).s = MVM_string_concatenate(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(repeat_s):
                GET_REG(cur_op, 0).s = MVM_string_repeat(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(substr_s):
                GET_REG(cur_op, 0).s = MVM_string_substring(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(index_s):
                GET_REG(cur_op, 0).i64 = MVM_string_index(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(graphs_s):
                GET_REG(cur_op, 0).i64 = MVM_string_graphs(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(codes_s):
                GET_REG(cur_op, 0).i64 = MVM_string_codes(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(getcp_s):
                GET_REG(cur_op, 0).i64 = MVM_string_get_grapheme_at(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(indexcp_s):
                GET_REG(cur_op, 0).i64 = MVM_string_index_of_grapheme(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(uc):
                GET_REG(cur_op, 0).s = MVM_string_uc(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(lc):
                GET_REG(cur_op, 0).s = MVM_string_lc(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(tc):
                GET_REG(cur_op, 0).s = MVM_string_tc(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(split):
                GET_REG(cur_op, 0).o = MVM_string_split(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(join):
                GET_REG(cur_op, 0).s = MVM_string_join(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(getcpbyname):
                GET_REG(cur_op, 0).i64 = MVM_unicode_lookup_by_name(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(indexat):
                /* branches on *failure* to match in the constant string, to save an instruction in regexes */
                if (MVM_string_char_at_in_string(tc, GET_REG(cur_op, 0).s,
                        GET_REG(cur_op, 2).i64,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 4))) >= 0)
                    cur_op += 12;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 8);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(indexnat):
                /* branches on *failure* to match in the constant string, to save an instruction in regexes */
                if (MVM_string_char_at_in_string(tc, GET_REG(cur_op, 0).s,
                        GET_REG(cur_op, 2).i64,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 4))) == -1)
                    cur_op += 12;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 8);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(unipropcode):
                GET_REG(cur_op, 0).i64 = (MVMint64)MVM_unicode_name_to_property_code(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(unipvalcode):
                GET_REG(cur_op, 0).i64 = (MVMint64)MVM_unicode_name_to_property_value_code(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(hasuniprop):
                GET_REG(cur_op, 0).i64 = MVM_string_offset_has_unicode_property_value(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64,
                    GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(hasunipropc):
                GET_REG(cur_op, 0).i64 = MVM_string_offset_has_unicode_property_value(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, (MVMint64)GET_UI16(cur_op, 6),
                    (MVMint64)GET_UI16(cur_op, 8));
                cur_op += 10;
                goto NEXT;
            OP(chars):
                GET_REG(cur_op, 0).i64 = MVM_string_graphs(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(chr):
                GET_REG(cur_op, 0).s = MVM_string_chr(tc, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(ordfirst): {
                GET_REG(cur_op, 0).i64 = MVM_string_ord_at(tc, GET_REG(cur_op, 2).s, 0);
                cur_op += 4;
                goto NEXT;
            }
            OP(ordat): {
                GET_REG(cur_op, 0).i64 = MVM_string_ord_at(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            }
            OP(rindexfrom):
                GET_REG(cur_op, 0).i64 = MVM_string_index_from_end(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(escape):
                GET_REG(cur_op, 0).s = MVM_string_escape(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(flip):
                GET_REG(cur_op, 0).s = MVM_string_flip(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(setbuffersize_fh):
                MVM_io_set_buffer_size(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(iscclass):
                GET_REG(cur_op, 0).i64 = MVM_string_is_cclass(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(findcclass):
                GET_REG(cur_op, 0).i64 = MVM_string_find_cclass(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(findnotcclass):
                GET_REG(cur_op, 0).i64 = MVM_string_find_not_cclass(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(nfafromstatelist):
                GET_REG(cur_op, 0).o = MVM_nfa_from_statelist(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(nfarunproto):
                GET_REG(cur_op, 0).o = MVM_nfa_run_proto(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(nfarunalt):
                MVM_nfa_run_alt(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o,
                    GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(radix):
                GET_REG(cur_op, 0).o = MVM_radix(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(encode):
                GET_REG(cur_op, 0).o = MVM_string_encode_to_buf(tc, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).o, NULL);
                cur_op += 8;
                goto NEXT;
            OP(decode):
                GET_REG(cur_op, 0).s = MVM_string_decode_from_buf(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(istrue_s):
                GET_REG(cur_op, 0).i64 = MVM_coerce_istrue_s(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(isfalse_s):
                GET_REG(cur_op, 0).i64 = MVM_coerce_istrue_s(tc, GET_REG(cur_op, 2).s) ? 0 : 1;
                cur_op += 4;
                goto NEXT;
            OP(null):
                GET_REG(cur_op, 0).o = tc->instance->VMNull;
                cur_op += 2;
                goto NEXT;
            OP(isnull): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = MVM_is_null(tc, obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(ifnonnull):
                if (MVM_is_null(tc, GET_REG(cur_op, 0).o))
                    cur_op += 6;
                else
                    cur_op = bytecode_start + GET_UI32(cur_op, 2);
                GC_SYNC_POINT(tc);
                goto NEXT;
            OP(findmeth): {
                /* Increment PC first, as we may make a method call. */
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = MVM_cu_string(tc, cu, GET_UI32(cur_op, 4));
                cur_op += 8;
                MVM_6model_find_method(tc, obj, name, res, 1);
                goto NEXT;
            }
            OP(findmeth_s):  {
                /* Increment PC first, as we may make a method call. */
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = GET_REG(cur_op, 4).s;
                cur_op += 6;
                MVM_6model_find_method(tc, obj, name, res, 1);
                goto NEXT;
            }
            OP(can): {
                /* Increment PC first, as we may make a method call. */
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = MVM_cu_string(tc, cu, GET_UI32(cur_op, 4));
                cur_op += 8;
                MVM_6model_can_method(tc, obj, name, res);
                goto NEXT;
            }
            OP(can_s): {
                /* Increment PC first, as we may make a method call. */
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = GET_REG(cur_op, 4).s;
                cur_op += 6;
                MVM_6model_can_method(tc, obj, name, res);
                goto NEXT;
            }
            OP(create): {
                /* Ordering here matters. We write the object into the
                 * register before calling initialize. This is because
                 * if initialize allocates, obj may have moved after
                 * we called it. Note that type is never used after
                 * the initial allocate call also. This saves us having
                 * to put things on the temporary stack. The GC will
                 * know to update it in the register if it moved. */
                MVMObject *type = GET_REG(cur_op, 2).o;
                MVMObject *obj  = REPR(type)->allocate(tc, STABLE(type));
                GET_REG(cur_op, 0).o = obj;
                if (REPR(obj)->initialize)
                    REPR(obj)->initialize(tc, STABLE(obj), obj, OBJECT_BODY(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(clone): {
                MVMObject *value = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(value)) {
                    MVMROOT(tc, value, {
                        MVMObject *cloned = REPR(value)->allocate(tc, STABLE(value));
                        /* Ordering here matters. We write the object into the
                        * register before calling copy_to. This is because
                        * if copy_to allocates, obj may have moved after
                        * we called it. This saves us having to put things on
                        * the temporary stack. The GC will know to update it
                        * in the register if it moved. */
                        GET_REG(cur_op, 0).o = cloned;
                        REPR(value)->copy_to(tc, STABLE(value), OBJECT_BODY(value), cloned, OBJECT_BODY(cloned));
                    });
                }
                else {
                    GET_REG(cur_op, 0).o = value;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(isconcrete): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && IS_CONCRETE(obj) ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(rebless):
                if (!REPR(GET_REG(cur_op, 2).o)->change_type) {
                    MVM_exception_throw_adhoc(tc, "This REPR cannot change type");
                }
                REPR(GET_REG(cur_op, 2).o)->change_type(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).o;
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 6;
                MVM_spesh_deopt_all(tc);
                goto NEXT;
            OP(istype): {
                /* Increment PC first, as we may make a method call. */
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMObject   *type = GET_REG(cur_op, 4).o;
                cur_op += 6;
                MVM_6model_istype(tc, obj, type, res);
                goto NEXT;
            }
            OP(objprimspec): {
                MVMObject *type = GET_REG(cur_op, 2).o;
                if (type) {
                    const MVMStorageSpec *ss = REPR(type)->get_storage_spec(tc, STABLE(type));
                    GET_REG(cur_op, 0).i64 = ss->boxed_primitive;
                }
                else {
                    GET_REG(cur_op, 0).i64 = 0;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(gethow):
                GET_REG(cur_op, 0).o = MVM_6model_get_how(tc,
                    STABLE(GET_REG(cur_op, 2).o));
                cur_op += 4;
                goto NEXT;
            OP(getwhat):
                GET_REG(cur_op, 0).o = STABLE(GET_REG(cur_op, 2).o)->WHAT;
                cur_op += 4;
                goto NEXT;
            OP(getwho): {
                MVMObject *who = STABLE(GET_REG(cur_op, 2).o)->WHO;
                GET_REG(cur_op, 0).o = who ? who : tc->instance->VMNull;
                cur_op += 4;
                goto NEXT;
            }
            OP(setwho): {
                MVMSTable *st = STABLE(GET_REG(cur_op, 2).o);
                MVM_ASSIGN_REF(tc, &(st->header), st->WHO, GET_REG(cur_op, 4).o);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).o;
                cur_op += 6;
                goto NEXT;
            }
            OP(reprname): {
                const MVMREPROps *repr = REPR(GET_REG(cur_op, 2).o);
                GET_REG(cur_op, 0).s = tc->instance->repr_list[repr->ID]->name;
                cur_op += 4;
                goto NEXT;
            }
            OP(getwhere):
                GET_REG(cur_op, 0).i64 = (uintptr_t)GET_REG(cur_op, 2).o;
                cur_op += 4;
                goto NEXT;
            OP(eqaddr):
                GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).o == GET_REG(cur_op, 4).o ? 1 : 0;
                cur_op += 6;
                goto NEXT;
            OP(bindattr_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 4)),
                    GET_I16(cur_op, 10), GET_REG(cur_op, 8), MVM_reg_int64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 12;
                goto NEXT;
            }
            OP(bindattr_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 4)),
                    GET_I16(cur_op, 10), GET_REG(cur_op, 8), MVM_reg_num64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 12;
                goto NEXT;
            }
            OP(bindattr_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 4)),
                    GET_I16(cur_op, 10), GET_REG(cur_op, 8), MVM_reg_str);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 12;
                goto NEXT;
            }
            OP(bindattr_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 4)),
                    GET_I16(cur_op, 10), GET_REG(cur_op, 8), MVM_reg_obj);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 12;
                goto NEXT;
            }
            OP(bindattrs_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    -1, GET_REG(cur_op, 6), MVM_reg_int64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 8;
                goto NEXT;
            }
            OP(bindattrs_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    -1, GET_REG(cur_op, 6), MVM_reg_num64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 8;
                goto NEXT;
            }
            OP(bindattrs_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    -1, GET_REG(cur_op, 6), MVM_reg_str);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 8;
                goto NEXT;
            }
            OP(bindattrs_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot bind attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.bind_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    -1, GET_REG(cur_op, 6), MVM_reg_obj);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 8;
                goto NEXT;
            }
            OP(getattr_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)),
                    GET_I16(cur_op, 10), &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 12;
                goto NEXT;
            }
            OP(getattr_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)),
                    GET_I16(cur_op, 10), &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 12;
                goto NEXT;
            }
            OP(getattr_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)),
                    GET_I16(cur_op, 10), &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 12;
                goto NEXT;
            }
            OP(getattr_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)),
                    GET_I16(cur_op, 10), &GET_REG(cur_op, 0), MVM_reg_obj);
                if (MVM_spesh_log_is_logging(tc))
                    MVM_spesh_log_type(tc, GET_REG(cur_op, 0).o);
                cur_op += 12;
                goto NEXT;
            }
            OP(getattrs_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    -1, &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 8;
                goto NEXT;
            }
            OP(getattrs_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    -1, &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 8;
                goto NEXT;
            }
            OP(getattrs_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    -1, &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 8;
                goto NEXT;
            }
            OP(getattrs_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                REPR(obj)->attr_funcs.get_attribute(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    -1, &GET_REG(cur_op, 0), MVM_reg_obj);
                if (MVM_spesh_log_is_logging(tc))
                    MVM_spesh_log_type(tc, GET_REG(cur_op, 0).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(attrinited): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot look up attributes in a %s type object", MVM_6model_get_debug_name(tc, obj));
                GET_REG(cur_op, 0).i64 = REPR(obj)->attr_funcs.is_attribute_initialized(tc,
                    STABLE(obj), OBJECT_BODY(obj),
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s, MVM_NO_HINT);
                cur_op += 8;
                goto NEXT;
            }
            OP(box_i): {
                MVM_box_int(tc, GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).o,
                            &GET_REG(cur_op, 0));
                cur_op += 6;
                goto NEXT;
            }
            OP(box_n): {
                MVM_box_num(tc, GET_REG(cur_op, 2).n64, GET_REG(cur_op, 4).o,
                            &GET_REG(cur_op, 0));
                cur_op += 6;
                goto NEXT;
            }
            OP(box_s): {
                 MVM_box_str(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o, &GET_REG(cur_op, 0));
                 cur_op += 6;
                 goto NEXT;
            }
            OP(unbox_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot unbox a type object (%s) to an int.", MVM_6model_get_debug_name(tc, obj));
                GET_REG(cur_op, 0).i64 = REPR(obj)->box_funcs.get_int(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(unbox_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot unbox a type object (%s) to a num.", MVM_6model_get_debug_name(tc, obj));
                GET_REG(cur_op, 0).n64 = REPR(obj)->box_funcs.get_num(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(unbox_s):
                GET_REG(cur_op, 0).s = MVM_unbox_str(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(atpos_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.at_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                    &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 6;
                goto NEXT;
            }
            OP(atpos_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.at_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                    &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 6;
                goto NEXT;
            }
            OP(atpos_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.at_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                    &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 6;
                goto NEXT;
            }
            OP(atpos_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj))
                    REPR(obj)->pos_funcs.at_pos(tc, STABLE(obj), obj,
                        OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                        &GET_REG(cur_op, 0), MVM_reg_obj);
                else
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                cur_op += 6;
                goto NEXT;
            }
            OP(bindpos_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.bind_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                    GET_REG(cur_op, 4), MVM_reg_int64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 6;
                goto NEXT;
            }
            OP(bindpos_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.bind_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                    GET_REG(cur_op, 4), MVM_reg_num64);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 6;
                goto NEXT;
            }
            OP(bindpos_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.bind_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                    GET_REG(cur_op, 4), MVM_reg_str);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 6;
                goto NEXT;
            }
            OP(bindpos_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.bind_pos(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                    GET_REG(cur_op, 4), MVM_reg_obj);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 6;
                goto NEXT;
            }
            OP(push_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.push(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_int64);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(push_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.push(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_num64);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(push_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.push(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_str);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(push_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.push(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_obj);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(pop_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.pop(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 4;
                goto NEXT;
            }
            OP(pop_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.pop(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 4;
                goto NEXT;
            }
            OP(pop_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.pop(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 4;
                goto NEXT;
            }
            OP(pop_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.pop(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(shift_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.shift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 4;
                goto NEXT;
            }
            OP(shift_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.shift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 4;
                goto NEXT;
            }
            OP(shift_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.shift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 4;
                goto NEXT;
            }
            OP(shift_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->pos_funcs.shift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(unshift_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.unshift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_int64);
                cur_op += 4;
                goto NEXT;
            }
            OP(unshift_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.unshift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_num64);
                cur_op += 4;
                goto NEXT;
            }
            OP(unshift_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.unshift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_str);
                cur_op += 4;
                goto NEXT;
            }
            OP(unshift_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.unshift(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(splice): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                REPR(obj)->pos_funcs.splice(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(setelemspos): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->pos_funcs.set_elems(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            }
            OP(existspos): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                GET_REG(cur_op, 0).i64 = MVM_repr_exists_pos(tc,
                    obj, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            }
            OP(atkey_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.at_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 4).s, &GET_REG(cur_op, 0), MVM_reg_int64);
                cur_op += 6;
                goto NEXT;
            }
            OP(atkey_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.at_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 4).s, &GET_REG(cur_op, 0), MVM_reg_num64);
                cur_op += 6;
                goto NEXT;
            }
            OP(atkey_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.at_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 4).s, &GET_REG(cur_op, 0), MVM_reg_str);
                cur_op += 6;
                goto NEXT;
            }
            OP(atkey_o): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj))
                    REPR(obj)->ass_funcs.at_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                        (MVMObject *)GET_REG(cur_op, 4).s, &GET_REG(cur_op, 0), MVM_reg_obj);
                else
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                cur_op += 6;
                goto NEXT;
            }
            OP(bindkey_i): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.bind_key(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4), MVM_reg_int64);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o); /* read register, obj may have moved in GC */
                cur_op += 6;
                goto NEXT;
            }
            OP(bindkey_n): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.bind_key(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4), MVM_reg_num64);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o); /* read register, obj may have moved in GC */
                cur_op += 6;
                goto NEXT;
            }
            OP(bindkey_s): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.bind_key(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4), MVM_reg_str);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o); /* read register, obj may have moved in GC */
                cur_op += 6;
                goto NEXT;
            }
            OP(bindkey_o): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.bind_key(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4), MVM_reg_obj);
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o); /* read register, obj may have moved in GC */
                cur_op += 6;
                goto NEXT;
            }
            OP(existskey): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                GET_REG(cur_op, 0).i64 = REPR(obj)->ass_funcs.exists_key(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(deletekey): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                CHECK_CONC(obj);
                REPR(obj)->ass_funcs.delete_key(tc, STABLE(obj), obj,
                    OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s);
                MVM_SC_WB_OBJ(tc, obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(elems): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                GET_REG(cur_op, 0).i64 = (MVMint64)REPR(obj)->elems(tc, STABLE(obj), obj, OBJECT_BODY(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(knowhow):
                GET_REG(cur_op, 0).o = tc->instance->KnowHOW;
                cur_op += 2;
                goto NEXT;
            OP(knowhowattr):
                GET_REG(cur_op, 0).o = tc->instance->KnowHOWAttribute;
                cur_op += 2;
                goto NEXT;
            OP(newtype): {
                MVMObject *how = GET_REG(cur_op, 2).o;
                MVMString *repr_name = GET_REG(cur_op, 4).s;
                const MVMREPROps *repr = MVM_repr_get_by_name(tc, repr_name);
                GET_REG(cur_op, 0).o = repr->type_object_for(tc, how);
                cur_op += 6;
                goto NEXT;
            }
            OP(composetype): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->compose(tc, STABLE(obj), GET_REG(cur_op, 4).o);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).o;
                cur_op += 6;
                goto NEXT;
            }
            OP(setmethcache): {
                MVMObject *iter = MVM_iter(tc, GET_REG(cur_op, 2).o);
                MVMObject *cache;
                MVMSTable *stable;
                MVMROOT(tc, iter, {
                    cache = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTHash);
                });

                while (MVM_iter_istrue(tc, (MVMIter *)iter)) {
                    MVMRegister result;
                    REPR(iter)->pos_funcs.shift(tc, STABLE(iter), iter,
                        OBJECT_BODY(iter), &result, MVM_reg_obj);
                    MVM_repr_bind_key_o(tc, cache, MVM_iterkey_s(tc, (MVMIter *)iter),
                        MVM_iterval(tc, (MVMIter *)iter));
                }

                stable = STABLE(GET_REG(cur_op, 0).o);
                MVM_ASSIGN_REF(tc, &(stable->header), stable->method_cache, cache);
                stable->method_cache_sc = NULL;
                MVM_SC_WB_ST(tc, stable);

                cur_op += 4;
                goto NEXT;
            }
            OP(setmethcacheauth): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                MVMint64 new_flags = STABLE(obj)->mode_flags & (~MVM_METHOD_CACHE_AUTHORITATIVE);
                MVMint64 flag = GET_REG(cur_op, 2).i64;
                if (flag != 0)
                    new_flags |= MVM_METHOD_CACHE_AUTHORITATIVE;
                STABLE(obj)->mode_flags = new_flags;
                MVM_SC_WB_ST(tc, STABLE(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(settypecache): {
                MVMObject *obj    = GET_REG(cur_op, 0).o;
                MVMObject *types  = GET_REG(cur_op, 2).o;
                MVMSTable *st     = STABLE(obj);
                MVMint64 i, elems = REPR(types)->elems(tc, STABLE(types), types, OBJECT_BODY(types));
                MVMObject **cache = MVM_malloc(sizeof(MVMObject *) * elems);
                for (i = 0; i < elems; i++) {
                    MVM_ASSIGN_REF(tc, &(st->header), cache[i], MVM_repr_at_pos_o(tc, types, i));
                }
                /* technically this free isn't thread safe */
                if (st->type_check_cache)
                    MVM_free(st->type_check_cache);
                st->type_check_cache = cache;
                st->type_check_cache_length = (MVMuint16)elems;
                MVM_SC_WB_ST(tc, st);
                cur_op += 4;
                goto NEXT;
            }
            OP(settypecheckmode): {
                MVMSTable *st = STABLE(GET_REG(cur_op, 0).o);
                st->mode_flags = GET_REG(cur_op, 2).i64 |
                    (st->mode_flags & (~MVM_TYPE_CHECK_CACHE_FLAG_MASK));
                MVM_SC_WB_ST(tc, st);
                cur_op += 4;
                goto NEXT;
            }
            OP(setboolspec): {
                MVMSTable            *st = GET_REG(cur_op, 0).o->st;
                MVMBoolificationSpec *bs = MVM_malloc(sizeof(MVMBoolificationSpec));
                MVMBoolificationSpec *orig_bs = st->boolification_spec;
                bs->mode = (MVMuint32)GET_REG(cur_op, 2).i64;
                MVM_ASSIGN_REF(tc, &(st->header), bs->method, GET_REG(cur_op, 4).o);
                st->boolification_spec = bs;
                MVM_free(orig_bs);
                cur_op += 6;
                goto NEXT;
            }
            OP(istrue): {
                /* Increment PC first then call coerce, since it may want to
                 * do an invocation. */
                MVMObject   *obj = GET_REG(cur_op, 2).o;
                MVMRegister *res = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_coerce_istrue(tc, obj, res, NULL, NULL, 0);
                goto NEXT;
            }
            OP(isfalse): {
                /* Increment PC first then call coerce, since it may want to
                 * do an invocation. */
                MVMObject   *obj = GET_REG(cur_op, 2).o;
                MVMRegister *res = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_coerce_istrue(tc, obj, res, NULL, NULL, 1);
                goto NEXT;
            }
            OP(bootint):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTInt;
                cur_op += 2;
                goto NEXT;
            OP(bootnum):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTNum;
                cur_op += 2;
                goto NEXT;
            OP(bootstr):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTStr;
                cur_op += 2;
                goto NEXT;
            OP(bootarray):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTArray;
                cur_op += 2;
                goto NEXT;
           OP(bootintarray):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTIntArray;
                cur_op += 2;
                goto NEXT;
            OP(bootnumarray):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTNumArray;
                cur_op += 2;
                goto NEXT;
            OP(bootstrarray):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTStrArray;
                cur_op += 2;
                goto NEXT;
            OP(boothash):
                GET_REG(cur_op, 0).o = tc->instance->boot_types.BOOTHash;
                cur_op += 2;
                goto NEXT;
            OP(isint): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && REPR(obj)->ID == MVM_REPR_ID_P6int ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(isnum): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && REPR(obj)->ID == MVM_REPR_ID_P6num ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(isstr): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && REPR(obj)->ID == MVM_REPR_ID_P6str ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(islist): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && REPR(obj)->ID == MVM_REPR_ID_VMArray ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(ishash): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = obj && REPR(obj)->ID == MVM_REPR_ID_MVMHash ? 1 : 0;
                cur_op += 4;
                goto NEXT;
            }
            OP(sethllconfig):
                MVM_hll_set_config(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(hllboxtype_i):
                GET_REG(cur_op, 0).o = cu->body.hll_config->int_box_type;
                cur_op += 2;
                goto NEXT;
            OP(hllboxtype_n):
                GET_REG(cur_op, 0).o = cu->body.hll_config->num_box_type;
                cur_op += 2;
                goto NEXT;
            OP(hllboxtype_s):
                GET_REG(cur_op, 0).o = cu->body.hll_config->str_box_type;
                cur_op += 2;
                goto NEXT;
            OP(hlllist):
                GET_REG(cur_op, 0).o = cu->body.hll_config->slurpy_array_type;
                cur_op += 2;
                goto NEXT;
            OP(hllhash):
                GET_REG(cur_op, 0).o = cu->body.hll_config->slurpy_hash_type;
                cur_op += 2;
                goto NEXT;
            OP(getcomp): {
                MVMObject *obj = tc->instance->compiler_registry;
                uv_mutex_lock(&tc->instance->mutex_compiler_registry);
                GET_REG(cur_op, 0).o = MVM_repr_at_key_o(tc, obj, GET_REG(cur_op, 2).s);
                uv_mutex_unlock(&tc->instance->mutex_compiler_registry);
                cur_op += 4;
                goto NEXT;
            }
            OP(bindcomp): {
                MVMObject *obj = tc->instance->compiler_registry;
                uv_mutex_lock(&tc->instance->mutex_compiler_registry);
                REPR(obj)->ass_funcs.bind_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 2).s, GET_REG(cur_op, 4), MVM_reg_obj);
                uv_mutex_unlock(&tc->instance->mutex_compiler_registry);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 4).o;
                cur_op += 6;
                goto NEXT;
            }
            OP(getcurhllsym): {
                MVMString *hll_name = tc->cur_frame->static_info->body.cu->body.hll_name;
                GET_REG(cur_op, 0).o = MVM_hll_sym_get(tc, hll_name, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            }
            OP(bindcurhllsym): {
                MVMObject *syms = tc->instance->hll_syms, *hash;
                MVMString *hll_name = tc->cur_frame->static_info->body.cu->body.hll_name;
                uv_mutex_lock(&tc->instance->mutex_hll_syms);
                hash = MVM_repr_at_key_o(tc, syms, hll_name);
                if (MVM_is_null(tc, hash)) {
                    hash = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTHash);
                    /* must re-get syms in case it moved */
                    syms = tc->instance->hll_syms;
                    hll_name = tc->cur_frame->static_info->body.cu->body.hll_name;
                    MVM_repr_bind_key_o(tc, syms, hll_name, hash);
                }
                MVM_repr_bind_key_o(tc, hash, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 4).o;
                uv_mutex_unlock(&tc->instance->mutex_hll_syms);
                cur_op += 6;
                goto NEXT;
            }
            OP(gethllsym):
                GET_REG(cur_op, 0).o = MVM_hll_sym_get(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(bindhllsym): {
                MVMObject *syms     = tc->instance->hll_syms;
                MVMString *hll_name = GET_REG(cur_op, 0).s;
                MVMObject *hash;
                uv_mutex_lock(&tc->instance->mutex_hll_syms);
                hash = MVM_repr_at_key_o(tc, syms, hll_name);
                if (MVM_is_null(tc, hash)) {
                    hash = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTHash);
                    /* must re-get syms and HLL name in case it moved */
                    syms = tc->instance->hll_syms;
                    hll_name = GET_REG(cur_op, 0).s;
                    MVM_repr_bind_key_o(tc, syms, hll_name, hash);
                }
                MVM_repr_bind_key_o(tc, hash, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o);
                uv_mutex_unlock(&tc->instance->mutex_hll_syms);
                cur_op += 6;
                goto NEXT;
            }
            OP(settypehll):
                STABLE(GET_REG(cur_op, 0).o)->hll_owner = MVM_hll_get_config_for(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(settypehllrole):
                STABLE(GET_REG(cur_op, 0).o)->hll_role = GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(hllize): {
                /* Increment PC before mapping, as it may invoke. */
                MVMRegister *res_reg = &GET_REG(cur_op, 0);
                MVMObject   *mapee   = GET_REG(cur_op, 2).o;
                cur_op += 4;
                MVM_hll_map(tc, mapee, MVM_hll_current(tc), res_reg);
                goto NEXT;
            }
            OP(hllizefor): {
                /* Increment PC before mapping, as it may invoke. */
                MVMRegister *res_reg = &GET_REG(cur_op, 0);
                MVMObject   *mapee   = GET_REG(cur_op, 2).o;
                MVMString   *hll     = GET_REG(cur_op, 4).s;
                cur_op += 6;
                MVM_hll_map(tc, mapee, MVM_hll_get_config_for(tc, hll), res_reg);
                goto NEXT;
            }
            OP(usecompileehllconfig):
                MVM_hll_enter_compilee_mode(tc);
                goto NEXT;
            OP(usecompilerhllconfig):
                MVM_hll_leave_compilee_mode(tc);
                goto NEXT;
            OP(iter): {
                GET_REG(cur_op, 0).o = MVM_iter(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(iterkey_s): {
                MVMIter *obj = (MVMIter *)GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                GET_REG(cur_op, 0).s = MVM_iterkey_s(tc, obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(iterval): {
                MVMIter *obj = (MVMIter *)GET_REG(cur_op, 2).o;
                CHECK_CONC(obj);
                GET_REG(cur_op, 0).o = MVM_iterval(tc, obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(getcodename): {
                MVMObject *co = GET_REG(cur_op, 2).o;
                if (REPR(co)->ID != MVM_REPR_ID_MVMCode || !IS_CONCRETE(co))
                    MVM_exception_throw_adhoc(tc, "getcodename requires a concrete code object");
                GET_REG(cur_op, 0).s = ((MVMCode *)co)->body.name;
                cur_op += 4;
                goto NEXT;
            }
            OP(iscoderef):
                GET_REG(cur_op, 0).i64 = !GET_REG(cur_op, 2).o ||
                    STABLE(GET_REG(cur_op, 2).o)->invoke == MVM_6model_invoke_default ? 0 : 1;
                cur_op += 4;
                goto NEXT;
            OP(getcodeobj): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).o = MVM_frame_get_code_object(tc, (MVMCode *)obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(setcodeobj): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (REPR(obj)->ID == MVM_REPR_ID_MVMCode) {
                    MVM_ASSIGN_REF(tc, &(obj->header), ((MVMCode *)obj)->body.code_object,
                        GET_REG(cur_op, 2).o);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "setcodeobj needs a code ref");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(setcodename): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                if (REPR(obj)->ID == MVM_REPR_ID_MVMCode) {
                    MVM_ASSIGN_REF(tc, &(obj->header), ((MVMCode *)obj)->body.name,
                        GET_REG(cur_op, 2).s);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "setcodename needs a code ref");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(forceouterctx): {
                MVMObject *obj = GET_REG(cur_op, 0).o, *ctx = GET_REG(cur_op, 2).o;
                MVMFrame *orig;
                MVMFrame *context;
                MVMStaticFrame *sf;
                if (REPR(obj)->ID != MVM_REPR_ID_MVMCode || !IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "forceouterctx needs a code ref");
                if (REPR(ctx)->ID != MVM_REPR_ID_MVMContext || !IS_CONCRETE(ctx))
                    MVM_exception_throw_adhoc(tc, "forceouterctx needs a context");
                context = MVM_context_get_frame_or_outer(tc, (MVMContext *)ctx);
                if (!context)
                    MVM_exception_throw_adhoc(tc, "Failed to resolve context for forceouterctx");

                orig = ((MVMCode *)obj)->body.outer;
                sf = ((MVMCode *)obj)->body.sf;

                MVM_ASSIGN_REF(tc, &(((MVMObject *)sf)->header), sf->body.outer, context->static_info);
                if (orig != context)
                    MVM_ASSIGN_REF(tc, &(obj->header), ((MVMCode *)obj)->body.outer, context);

                cur_op += 4;
                goto NEXT;
            }
            OP(setinvokespec): {
                MVMObject *obj = GET_REG(cur_op, 0).o, *ch = GET_REG(cur_op, 2).o,
                    *invocation_handler = GET_REG(cur_op, 6).o;
                MVMString *name = GET_REG(cur_op, 4).s;
                MVMInvocationSpec *is = MVM_calloc(1, sizeof(MVMInvocationSpec));
                MVMSTable *st = STABLE(obj);
                MVM_ASSIGN_REF(tc, &(st->header), is->class_handle, ch);
                MVM_ASSIGN_REF(tc, &(st->header), is->attr_name, name);
                if (ch && name)
                    is->hint = REPR(ch)->attr_funcs.hint_for(tc, STABLE(ch), ch, name);
                MVM_ASSIGN_REF(tc, &(st->header), is->invocation_handler, invocation_handler);
                /* XXX not thread safe, but this should occur on non-shared objects anyway... */
                if (st->invocation_spec)
                    MVM_free(st->invocation_spec);
                st->invocation_spec = is;
                cur_op += 8;
                goto NEXT;
            }
            OP(isinvokable): {
                MVMSTable *st = STABLE(GET_REG(cur_op, 2).o);
                GET_REG(cur_op, 0).i64 = st->invoke == MVM_6model_invoke_default
                    ? (st->invocation_spec ? 1 : 0)
                    : 1;
                cur_op += 4;
                goto NEXT;
            }
            OP(freshcoderef): {
                MVMObject * const cr = GET_REG(cur_op, 2).o;
                MVMCode *ncr;
                if (REPR(cr)->ID != MVM_REPR_ID_MVMCode)
                    MVM_exception_throw_adhoc(tc, "freshcoderef requires a coderef");
                ncr = (MVMCode *)(GET_REG(cur_op, 0).o = MVM_repr_clone(tc, cr));
                MVMROOT(tc, ncr, {
                    MVMStaticFrame *nsf;
                    if (!ncr->body.sf->body.fully_deserialized)
                        MVM_bytecode_finish_frame(tc, ncr->body.sf->body.cu, ncr->body.sf, 0);
                    nsf = (MVMStaticFrame *)MVM_repr_clone(tc,
                        (MVMObject *)ncr->body.sf);
                    MVM_ASSIGN_REF(tc, &(ncr->common.header), ncr->body.sf, nsf);
                    MVM_ASSIGN_REF(tc, &(ncr->common.header), ncr->body.sf->body.static_code, ncr);
                });
                cur_op += 4;
                goto NEXT;
            }
            OP(markcodestatic): {
                MVMObject * const cr = GET_REG(cur_op, 0).o;
                CHECK_CONC(cr);
                if (REPR(cr)->ID != MVM_REPR_ID_MVMCode)
                    MVM_exception_throw_adhoc(tc, "markcodestatic requires a coderef");
                ((MVMCode *)cr)->body.is_static = 1;
                cur_op += 2;
                goto NEXT;
            }
            OP(markcodestub): {
                MVMObject * const cr = GET_REG(cur_op, 0).o;
                CHECK_CONC(cr);
                if (REPR(cr)->ID != MVM_REPR_ID_MVMCode)
                    MVM_exception_throw_adhoc(tc, "markcodestub requires a coderef");
                ((MVMCode *)cr)->body.is_compiler_stub = 1;
                cur_op += 2;
                goto NEXT;
            }
            OP(getstaticcode): {
                MVMObject * const cr = GET_REG(cur_op, 2).o;
                CHECK_CONC(cr);
                if (REPR(cr)->ID != MVM_REPR_ID_MVMCode)
                    MVM_exception_throw_adhoc(tc, "getstaticcode requires a static coderef");
                GET_REG(cur_op, 0).o = (MVMObject *)((MVMCode *)cr)->body.sf->body.static_code;
                cur_op += 4;
                goto NEXT;
            }
            OP(getcodecuid): {
                MVMObject * const cr = GET_REG(cur_op, 2).o;
                CHECK_CONC(cr);
                if (REPR(cr)->ID != MVM_REPR_ID_MVMCode || !IS_CONCRETE(cr))
                    MVM_exception_throw_adhoc(tc, "getcodecuid requires a static coderef");
                GET_REG(cur_op, 0).s = ((MVMCode *)cr)->body.sf->body.cuuid;
                cur_op += 4;
                goto NEXT;
            }
            OP(setdispatcher):
                tc->cur_dispatcher = GET_REG(cur_op, 0).o;
                tc->cur_dispatcher_for = NULL;
                cur_op += 2;
                goto NEXT;
            OP(takedispatcher): {
                MVMObject *disp = tc->cur_dispatcher;
                MVMObject *disp_for = tc->cur_dispatcher_for;
                MVMObject *cur_code = tc->cur_frame->code_ref;
                if (disp && (!disp_for || disp_for == cur_code)) {
                    GET_REG(cur_op, 0).o = disp;
                    tc->cur_dispatcher = NULL;
                }
                else {
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                }
                cur_op += 2;
                goto NEXT;
            }
            OP(assign): {
                MVMObject *cont  = GET_REG(cur_op, 0).o;
                MVMObject *obj = GET_REG(cur_op, 2).o;
                const MVMContainerSpec *spec = STABLE(cont)->container_spec;
                cur_op += 4;
                CHECK_CONC(cont);
                if (spec) {
                    spec->store(tc, cont, obj);
                } else {
                    MVM_exception_throw_adhoc(tc, "Cannot assign to an immutable value");
                }
                goto NEXT;
            }
            OP(assignunchecked): {
                MVMObject *cont  = GET_REG(cur_op, 0).o;
                MVMObject *obj = GET_REG(cur_op, 2).o;
                const MVMContainerSpec *spec = STABLE(cont)->container_spec;
                cur_op += 4;
                CHECK_CONC(cont);
                if (spec) {
                    spec->store_unchecked(tc, cont, obj);
                } else {
                    MVM_exception_throw_adhoc(tc, "Cannot assign to an immutable value");
                }
                goto NEXT;
            }
            OP(iscont): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = MVM_is_null(tc, obj) || STABLE(obj)->container_spec == NULL ? 0 : 1;
                cur_op += 4;
                goto NEXT;
            }
            OP(decont): {
                MVMuint8 *prev_op = cur_op;
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                if (obj && IS_CONCRETE(obj) && STABLE(obj)->container_spec) {
                    STABLE(obj)->container_spec->fetch(tc, obj, r);
                    if (MVM_spesh_log_is_logging(tc))
                        MVM_spesh_log_decont(tc, prev_op, r->o);
                }
                else {
                    r->o = obj;
                }
                goto NEXT;
            }
            OP(setcontspec): {
                MVMSTable *st   = STABLE(GET_REG(cur_op, 0).o);
                MVMString *name = GET_REG(cur_op, 2).s;
                const MVMContainerConfigurer *cc = MVM_6model_get_container_config(tc, name);
                if (cc == NULL) {
                    char *c_name = MVM_string_utf8_encode_C_string(tc, name);
                    char *waste[] = { c_name, NULL };
                    MVM_exception_throw_adhoc_free(tc, waste, "Cannot use unknown container spec %s",
                        c_name);
                }
                if (st->container_spec)
                    MVM_exception_throw_adhoc(tc,
                        "Cannot change a type's container specification");

                cc->set_container_spec(tc, st);
                cc->configure_container_spec(tc, st, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(sha1):
                GET_REG(cur_op, 0).s = MVM_sha1(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(createsc):
                GET_REG(cur_op, 0).o = MVM_sc_create(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(scsetobj): {
                MVMObject *sc  = GET_REG(cur_op, 0).o;
                MVMObject *obj = GET_REG(cur_op, 4).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scsetobj");
                MVM_sc_set_object(tc, (MVMSerializationContext *)sc,
                    GET_REG(cur_op, 2).i64, obj);
                if (MVM_sc_get_stable_sc(tc, STABLE(obj)) == NULL) {
                    /* Need to claim the SC also; typical case for new type objects. */
                    MVMSTable *st = STABLE(obj);
                    MVM_sc_set_stable_sc(tc, st, (MVMSerializationContext *)sc);
                    MVM_sc_push_stable(tc, (MVMSerializationContext *)sc, st);
                }
                cur_op += 6;
                goto NEXT;
            }
            OP(scsetcode): {
                MVMObject *sc   = GET_REG(cur_op, 0).o;
                MVMObject *code = GET_REG(cur_op, 4).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scsetcode");
                MVM_sc_set_obj_sc(tc, code, (MVMSerializationContext *)sc);
                MVM_sc_set_code(tc, (MVMSerializationContext *)sc,
                    GET_REG(cur_op, 2).i64, code);
                cur_op += 6;
                goto NEXT;
            }
            OP(scgetobj): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scgetobj");
                GET_REG(cur_op, 0).o = MVM_sc_get_object(tc,
                    (MVMSerializationContext *)sc, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            }
            OP(scgethandle): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scgethandle");
                GET_REG(cur_op, 0).s = MVM_sc_get_handle(tc,
                    (MVMSerializationContext *)sc);
                cur_op += 4;
                goto NEXT;
            }
            OP(scgetobjidx): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scgetobjidx");
                GET_REG(cur_op, 0).i64 = MVM_sc_find_object_idx(tc,
                    (MVMSerializationContext *)sc, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(scsetdesc): {
                MVMObject *sc   = GET_REG(cur_op, 0).o;
                MVMString *desc = GET_REG(cur_op, 2).s;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scsetdesc");
                MVM_sc_set_description(tc, (MVMSerializationContext *)sc, desc);
                cur_op += 4;
                goto NEXT;
            }
            OP(scobjcount): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scobjcount");
                GET_REG(cur_op, 0).i64 = MVM_sc_get_object_count(tc,
                    (MVMSerializationContext *)sc);
                cur_op += 4;
                goto NEXT;
            }
            OP(setobjsc): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                MVMObject *sc  = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to setobjsc");
                MVM_sc_set_obj_sc(tc, obj, (MVMSerializationContext *)sc);
                cur_op += 4;
                goto NEXT;
            }
            OP(getobjsc):
                GET_REG(cur_op, 0).o = (MVMObject *)MVM_sc_get_obj_sc(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(serialize): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                MVMObject *obj = GET_REG(cur_op, 4).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to serialize");
                GET_REG(cur_op, 0).s = (MVMString *)MVM_serialization_serialize(
                    tc,
                    (MVMSerializationContext *)sc,
                    obj,
                    NULL
                );
                cur_op += 6;
                goto NEXT;
            }
            OP(deserialize): {
                MVMString *blob = GET_REG(cur_op, 0).s;
                MVMObject *sc   = GET_REG(cur_op, 2).o;
                MVMObject *sh   = GET_REG(cur_op, 4).o;
                MVMObject *cr   = GET_REG(cur_op, 6).o;
                MVMObject *conf = GET_REG(cur_op, 8).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to deserialize");
                MVM_serialization_deserialize(tc, (MVMSerializationContext *)sc,
                    sh, cr, conf, blob);
                cur_op += 10;
                goto NEXT;
            }
            OP(wval): {
                MVMuint16 dep = GET_UI16(cur_op, 2);
                MVMuint16 idx = GET_UI16(cur_op, 4);
                GET_REG(cur_op, 0).o = MVM_sc_get_sc_object(tc, cu, dep, idx);
                cur_op += 6;
                goto NEXT;
            }
            OP(wval_wide): {
                MVMuint16 dep = GET_UI16(cur_op, 2);
                MVMuint64 idx = MVM_BC_get_I64(cur_op, 4);
                GET_REG(cur_op, 0).o = MVM_sc_get_sc_object(tc, cu, dep, idx);
                cur_op += 12;
                goto NEXT;
            }
            OP(scwbdisable):
                GET_REG(cur_op, 0).i64 = ++tc->sc_wb_disable_depth;
                cur_op += 2;
                goto NEXT;
            OP(scwbenable):
                GET_REG(cur_op, 0).i64 = --tc->sc_wb_disable_depth;
                cur_op += 2;
                goto NEXT;
            OP(pushcompsc): {
                MVMObject * const sc  = GET_REG(cur_op, 0).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc, "Can only push an SCRef with pushcompsc");
                if (MVM_is_null(tc, tc->compiling_scs)) {
                    MVMROOT(tc, sc, {
                        tc->compiling_scs = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTArray);
                    });
                }
                MVM_repr_unshift_o(tc, tc->compiling_scs, sc);
                cur_op += 2;
                goto NEXT;
            }
            OP(popcompsc): {
                MVMObject * const scs = tc->compiling_scs;
                if (MVM_is_null(tc, scs) || MVM_repr_elems(tc, scs) == 0)
                    MVM_exception_throw_adhoc(tc, "No current compiling SC");
                GET_REG(cur_op, 0).o = MVM_repr_shift_o(tc, tc->compiling_scs);
                cur_op += 2;
                goto NEXT;
            }
            OP(scgetdesc): {
                MVMObject *sc = GET_REG(cur_op, 2).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to scgetdesc");
                GET_REG(cur_op, 0).s = MVM_sc_get_description(tc,
                    (MVMSerializationContext *)sc);
                cur_op += 4;
                goto NEXT;
            }
            OP(loadbytecode): {
                /* This op will end up returning into the runloop to run
                 * deserialization and load code, so make sure we're done
                 * processing this op really. */
                MVMString *filename = GET_REG(cur_op, 2).s;
                GET_REG(cur_op, 0).s = filename;
                cur_op += 4;

                /* Set up return (really continuation after load) address
                 * and enter bytecode loading process. */
                tc->cur_frame->return_address = cur_op;
                MVM_load_bytecode(tc, filename);
                goto NEXT;
            }
            OP(newmixintype): {
                MVMObject *how = GET_REG(cur_op, 2).o;
                MVMString *repr_name = GET_REG(cur_op, 4).s;
                const MVMREPROps *repr = MVM_repr_get_by_name(tc, repr_name);
                MVMObject *type = repr->type_object_for(tc, how);
                STABLE(type)->is_mixin_type = 1;
                GET_REG(cur_op, 0).o = type;
                cur_op += 6;
                goto NEXT;
            }
            OP(installconfprog):
                MVM_confprog_install(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(iscompunit): {
                MVMObject *maybe_cu = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = REPR(maybe_cu)->ID == MVM_REPR_ID_MVMCompUnit;
                cur_op += 4;
                goto NEXT;
            }
            OP(compunitmainline): {
                MVMObject *maybe_cu = GET_REG(cur_op, 2).o;
                CHECK_CONC(maybe_cu);
                if (REPR(maybe_cu)->ID == MVM_REPR_ID_MVMCompUnit) {
                    MVMCompUnit *cu = (MVMCompUnit *)maybe_cu;
                    GET_REG(cur_op, 0).o = cu->body.coderefs[0];
                }
                else {
                    MVM_exception_throw_adhoc(tc, "compunitmainline requires an MVMCompUnit");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(compunitcodes): {
                MVMObject *     const result = MVM_repr_alloc_init(tc, MVM_hll_current(tc)->slurpy_array_type);
                MVMCompUnit * const maybe_cu = (MVMCompUnit *)GET_REG(cur_op, 2).o;
                CHECK_CONC(maybe_cu);
                if (REPR(maybe_cu)->ID == MVM_REPR_ID_MVMCompUnit) {
                    const MVMuint32 num_frames  = maybe_cu->body.num_frames;
                    MVMObject ** const coderefs = maybe_cu->body.coderefs;
                    MVMuint32 i;

                    for (i = 0; i < num_frames; i++) {
                        MVM_repr_push_o(tc, result, coderefs[i]);
                    }

                    GET_REG(cur_op, 0).o = result;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "compunitcodes requires an MVMCompUnit");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(ctx): {
                GET_REG(cur_op, 0).o = MVM_context_from_frame(tc, tc->cur_frame);
                cur_op += 2;
                goto NEXT;
            }
            OP(ctxouter): {
                MVMObject *ctx = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(ctx) || REPR(ctx)->ID != MVM_REPR_ID_MVMContext)
                    MVM_exception_throw_adhoc(tc, "ctxouter needs an MVMContext");
                GET_REG(cur_op, 0).o = MVM_context_apply_traversal(tc, (MVMContext *)ctx,
                        MVM_CTX_TRAV_OUTER);
                cur_op += 4;
                goto NEXT;
            }
            OP(ctxcaller): {
                MVMObject *ctx = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(ctx) || REPR(ctx)->ID != MVM_REPR_ID_MVMContext)
                    MVM_exception_throw_adhoc(tc, "ctxcaller needs an MVMContext");
                GET_REG(cur_op, 0).o = MVM_context_apply_traversal(tc, (MVMContext *)ctx,
                        MVM_CTX_TRAV_CALLER);
                cur_op += 4;
                goto NEXT;
            }
            OP(ctxlexpad): {
                MVMObject *this_ctx = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(this_ctx) || REPR(this_ctx)->ID != MVM_REPR_ID_MVMContext) {
                    MVM_exception_throw_adhoc(tc, "ctxlexpad needs an MVMContext");
                }
                GET_REG(cur_op, 0).o = this_ctx;
                cur_op += 4;
                goto NEXT;
            }
            OP(curcode):
                GET_REG(cur_op, 0).o = tc->cur_frame->code_ref;
                cur_op += 2;
                goto NEXT;
            OP(callercode):
                GET_REG(cur_op, 0).o = MVM_frame_caller_code(tc);
                cur_op += 2;
                goto NEXT;
            OP(add_I):
                GET_REG(cur_op, 0).o = MVM_bigint_add(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            OP(sub_I):
                GET_REG(cur_op, 0).o = MVM_bigint_sub(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            OP(mul_I):
                GET_REG(cur_op, 0).o = MVM_bigint_mul(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            OP(div_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_div(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(mod_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_mod(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(neg_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_neg(tc, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(abs_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_abs(tc, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(cmp_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(eq_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_EQ == MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(ne_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_EQ != MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(lt_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_LT == MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(le_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_GT != MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(gt_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_GT == MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(ge_I): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).i64 = MP_LT != MVM_bigint_cmp(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(bor_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_or(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(bxor_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_xor(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(band_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_and(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(bnot_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_not(tc, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(blshift_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_shl(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(brshift_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_shr(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(pow_I):
                GET_REG(cur_op, 0).o = MVM_bigint_pow(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o);
                cur_op += 10;
                goto NEXT;
            OP(gcd_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_gcd(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(lcm_I):
                GET_REG(cur_op, 0).o = MVM_bigint_lcm(tc, GET_REG(cur_op, 6).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 8;
                goto NEXT;
            OP(expmod_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_expmod(tc, GET_REG(cur_op, 8).o, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o);
                cur_op += 10;
                goto NEXT;
            }
            OP(isprime_I): {
                MVMObject *a = GET_REG(cur_op, 2).o;
                MVMint64 b = GET_REG(cur_op, 4).i64;
                GET_REG(cur_op, 0).i64 = MVM_bigint_is_prime(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(rand_I): {
                GET_REG(cur_op, 0).o = MVM_bigint_rand(tc, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(coerce_In): {
                MVMObject *a = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).n64 = MVM_bigint_to_num(tc, a);
                cur_op += 4;
                goto NEXT;
            }
            OP(coerce_Is): {
                GET_REG(cur_op, 0).s = MVM_bigint_to_str(tc, GET_REG(cur_op, 2).o, 10);
                cur_op += 4;
                goto NEXT;
            }
            OP(coerce_nI): {
                GET_REG(cur_op, 0).o = MVM_bigint_from_num(tc, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 2).n64);
                cur_op += 6;
                goto NEXT;
            }
            OP(coerce_sI): {
                GET_REG(cur_op, 0).o = MVM_coerce_sI(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(isbig_I):
                GET_REG(cur_op, 0).i64 = MVM_bigint_is_big(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(bool_I):
                GET_REG(cur_op, 0).i64 = MVM_bigint_bool(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(base_I): {
                GET_REG(cur_op, 0).s = MVM_bigint_to_str(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            }
            OP(radix_I):
                GET_REG(cur_op, 0).o = MVM_bigint_radix(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(div_In): {
                MVMObject *a = GET_REG(cur_op, 2).o, *b = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).n64 = MVM_bigint_div_num(tc, a, b);
                cur_op += 6;
                goto NEXT;
            }
            OP(copy_f):
                MVM_file_copy(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(append_f):
                MVM_exception_throw_adhoc(tc, "append is not supported");
                goto NEXT;
            OP(rename_f):
                MVM_file_rename(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(delete_f):
                MVM_file_delete(tc, GET_REG(cur_op, 0).s);
                cur_op += 2;
                goto NEXT;
            OP(chmod_f):
                MVM_file_chmod(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(exists_f):
                GET_REG(cur_op, 0).i64 = MVM_file_exists(tc, GET_REG(cur_op, 2).s, 0);
                cur_op += 4;
                goto NEXT;
            OP(mkdir):
                MVM_dir_mkdir(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(rmdir):
                MVM_dir_rmdir(tc, GET_REG(cur_op, 0).s);
                cur_op += 2;
                goto NEXT;
            OP(open_dir):
                GET_REG(cur_op, 0).o = MVM_dir_open(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(read_dir):
                GET_REG(cur_op, 0).s = MVM_dir_read(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(close_dir):
                MVM_dir_close(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(open_fh):
                GET_REG(cur_op, 0).o = MVM_file_open_fh(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(close_fh):
                MVM_io_close(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(seek_fh):
                MVM_io_seek(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64,
                    GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(lock_fh):
                GET_REG(cur_op, 0).i64 = MVM_io_lock(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(unlock_fh):
                MVM_io_unlock(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(sync_fh):
                MVM_io_flush(tc, GET_REG(cur_op, 0).o, 1);
                cur_op += 2;
                goto NEXT;
            OP(trunc_fh):
                MVM_io_truncate(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(eof_fh):
                GET_REG(cur_op, 0).i64 = MVM_io_eof(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(getstdin):
                if (MVM_is_null(tc, tc->instance->stdin_handle))
                    MVM_exception_throw_adhoc(tc, "STDIN filehandle was never initialized");
                GET_REG(cur_op, 0).o = tc->instance->stdin_handle;
                cur_op += 2;
                goto NEXT;
            OP(getstdout):
                if (MVM_is_null(tc, tc->instance->stdout_handle))
                    MVM_exception_throw_adhoc(tc, "STDOUT filehandle was never initialized");
                GET_REG(cur_op, 0).o = tc->instance->stdout_handle;
                cur_op += 2;
                goto NEXT;
            OP(getstderr):
                if (MVM_is_null(tc, tc->instance->stderr_handle))
                    MVM_exception_throw_adhoc(tc, "STDERR filehandle was never initialized");
                GET_REG(cur_op, 0).o = tc->instance->stderr_handle;
                cur_op += 2;
                goto NEXT;
            OP(connect_sk):
                MVM_io_connect(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).u16);
                cur_op += 8;
                goto NEXT;
            OP(socket):
                GET_REG(cur_op, 0).o = MVM_io_socket_create(tc, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(bind_sk):
                MVM_io_bind(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).u16, (MVMint32)GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(accept_sk):
                GET_REG(cur_op, 0).o = MVM_io_accept(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(decodetocodes):
            OP(encodefromcodes):
                MVM_exception_throw_adhoc(tc, "NYI");
            OP(print):
                MVM_string_print(tc, GET_REG(cur_op, 0).s);
                cur_op += 2;
                goto NEXT;
            OP(say):
                MVM_string_say(tc, GET_REG(cur_op, 0).s);
                cur_op += 2;
                goto NEXT;
            OP(tell_fh):
                GET_REG(cur_op, 0).i64 = MVM_io_tell(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(stat):
                GET_REG(cur_op, 0).i64 = MVM_file_stat(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, 0);
                cur_op += 6;
                goto NEXT;
            OP(tryfindmeth): {
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = MVM_cu_string(tc, cu, GET_UI32(cur_op, 4));
                cur_op += 8;
                MVM_6model_find_method(tc, obj, name, res, 0);
                goto NEXT;
            }
            OP(tryfindmeth_s):  {
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *obj  = GET_REG(cur_op, 2).o;
                MVMString   *name = GET_REG(cur_op, 4).s;
                cur_op += 6;
                MVM_6model_find_method(tc, obj, name, res, 0);
                goto NEXT;
            }
            OP(chdir):
                MVM_dir_chdir(tc, GET_REG(cur_op, 0).s);
                cur_op += 2;
                goto NEXT;
            OP(srand):
                MVM_proc_seed(tc, GET_REG(cur_op, 0).i64);
                cur_op += 2;
                goto NEXT;
            OP(rand_i):
                GET_REG(cur_op, 0).i64 = MVM_proc_rand_i(tc);
                cur_op += 2;
                goto NEXT;
            OP(rand_n):
                GET_REG(cur_op, 0).n64 = MVM_proc_rand_n(tc);
                cur_op += 2;
                goto NEXT;
            OP(time_i):
                GET_REG(cur_op, 0).i64 = MVM_proc_time_i(tc);
                cur_op += 2;
                goto NEXT;
            OP(sleep): {
                MVM_gc_mark_thread_blocked(tc);
                MVM_platform_sleep(GET_REG(cur_op, 0).n64);
                MVM_gc_mark_thread_unblocked(tc);
                cur_op += 2;
                goto NEXT;
            }
            OP(newthread):
                GET_REG(cur_op, 0).o = MVM_thread_new(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(threadjoin):
                MVM_thread_join(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(time_n):
                GET_REG(cur_op, 0).n64 = MVM_proc_time_n(tc);
                cur_op += 2;
                goto NEXT;
            OP(exit): {
                MVMint64 exit_code = GET_REG(cur_op, 0).i64;
                MVM_io_flush_standard_handles(tc);
                exit(exit_code);
            }
            OP(cwd):
                GET_REG(cur_op, 0).s = MVM_dir_cwd(tc);
                cur_op += 2;
                goto NEXT;
            OP(clargs):
                GET_REG(cur_op, 0).o = MVM_proc_clargs(tc);
                cur_op += 2;
                goto NEXT;
            OP(getenvhash):
                GET_REG(cur_op, 0).o = MVM_proc_getenvhash(tc);
                cur_op += 2;
                goto NEXT;
            OP(loadlib): {
                MVMString *name = GET_REG(cur_op, 0).s;
                MVMString *path = GET_REG(cur_op, 2).s;
                MVM_dll_load(tc, name, path);
                cur_op += 4;
                goto NEXT;
            }
            OP(freelib): {
                MVMString *name = GET_REG(cur_op, 0).s;
                MVM_dll_free(tc, name);
                cur_op += 2;
                goto NEXT;
            }
            OP(findsym): {
                MVMString *lib = GET_REG(cur_op, 2).s;
                MVMString *sym = GET_REG(cur_op, 4).s;
                MVMObject *obj = MVM_dll_find_symbol(tc, lib, sym);
                if (MVM_is_null(tc, obj))
                    MVM_exception_throw_adhoc(tc, "symbol not found in DLL");

                GET_REG(cur_op, 0).o = obj;
                cur_op += 6;
                goto NEXT;
            }
            OP(dropsym): {
                MVM_dll_drop_symbol(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            }
            OP(loadext): {
                MVMString *lib = GET_REG(cur_op, 0).s;
                MVMString *ext = GET_REG(cur_op, 2).s;
                MVM_ext_load(tc, lib, ext);
                cur_op += 4;
                goto NEXT;
            }
            OP(backendconfig):
                GET_REG(cur_op, 0).o = MVM_backend_config(tc);
                cur_op += 2;
                goto NEXT;
            OP(getlexouter): {
                GET_REG(cur_op, 0).o = MVM_frame_find_lexical_by_name_outer(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            }
            OP(getlexrel): {
                MVMObject *ctx  = GET_REG(cur_op, 2).o;
                if (REPR(ctx)->ID != MVM_REPR_ID_MVMContext || !IS_CONCRETE(ctx))
                    MVM_exception_throw_adhoc(tc, "getlexrel needs a context");
                GET_REG(cur_op, 0).o = MVM_context_lexical_lookup(tc, (MVMContext *)ctx,
                        GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(getlexreldyn): {
                MVMObject *ctx  = GET_REG(cur_op, 2).o;
                if (REPR(ctx)->ID != MVM_REPR_ID_MVMContext || !IS_CONCRETE(ctx))
                    MVM_exception_throw_adhoc(tc, "getlexreldyn needs a context");
                GET_REG(cur_op, 0).o = MVM_context_dynamic_lookup(tc, (MVMContext *)ctx,
                        GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(getlexrelcaller): {
                MVMObject   *ctx  = GET_REG(cur_op, 2).o;
                if (REPR(ctx)->ID != MVM_REPR_ID_MVMContext || !IS_CONCRETE(ctx))
                    MVM_exception_throw_adhoc(tc, "getlexrelcaller needs a context");
                GET_REG(cur_op, 0).o = MVM_context_caller_lookup(tc, (MVMContext *)ctx,
                        GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(getlexcaller): {
                MVMRegister *res = MVM_frame_find_lexical_by_name_rel_caller(tc,
                    GET_REG(cur_op, 2).s, tc->cur_frame->caller);
                GET_REG(cur_op, 0).o = res ? res->o : tc->instance->VMNull;
                cur_op += 4;
                goto NEXT;
            }
            OP(bitand_s):
                GET_REG(cur_op, 0).s = MVM_string_bitand(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(bitor_s):
                GET_REG(cur_op, 0).s = MVM_string_bitor(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(bitxor_s):
                GET_REG(cur_op, 0).s = MVM_string_bitxor(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(isnanorinf):
                GET_REG(cur_op, 0).i64 = MVM_num_isnanorinf(tc, GET_REG(cur_op, 2).n64);
                cur_op += 4;
                goto NEXT;
            OP(inf):
                GET_REG(cur_op, 0).n64 = MVM_num_posinf(tc);
                cur_op += 2;
                goto NEXT;
            OP(neginf):
                GET_REG(cur_op, 0).n64 = MVM_num_neginf(tc);
                cur_op += 2;
                goto NEXT;
            OP(nan):
                GET_REG(cur_op, 0).n64 = MVM_num_nan(tc);
                cur_op += 2;
                goto NEXT;
            OP(getpid):
                GET_REG(cur_op, 0).i64 = MVM_proc_getpid(tc);
                cur_op += 2;
                goto NEXT;
            OP(filereadable):
                GET_REG(cur_op, 0).i64 = MVM_file_isreadable(tc, GET_REG(cur_op, 2).s,0);
                cur_op += 4;
                goto NEXT;
            OP(filewritable):
                GET_REG(cur_op, 0).i64 = MVM_file_iswritable(tc, GET_REG(cur_op, 2).s,0);
                cur_op += 4;
                goto NEXT;
            OP(fileexecutable):
                GET_REG(cur_op, 0).i64 = MVM_file_isexecutable(tc, GET_REG(cur_op, 2).s,0);
                cur_op += 4;
                goto NEXT;
            OP(capturenamedshash): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(obj) && REPR(obj)->ID == MVM_REPR_ID_MVMCallCapture) {
                    MVMCallCapture *cc = (MVMCallCapture *)obj;
                    GET_REG(cur_op, 0).o = MVM_args_slurpy_named(tc, cc->body.apc);
                }
                else {
                    MVM_exception_throw_adhoc(tc, "capturehasnameds needs a MVMCallCapture");
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(read_fhb):
                MVM_io_read_bytes(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(write_fhb):
                MVM_io_write_bytes(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(replace):
                GET_REG(cur_op, 0).s = MVM_string_replace(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).s);
                cur_op += 10;
                goto NEXT;
            OP(newexception):
                GET_REG(cur_op, 0).o = (MVMObject *)MVM_repr_alloc_init(tc,
                    tc->instance->boot_types.BOOTException);
                cur_op += 2;
                goto NEXT;
            OP(permit):
                MVM_io_eventloop_permit(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64,
                       GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(backtrace):
                GET_REG(cur_op, 0).o = MVM_exception_backtrace(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(symlink):
                MVM_file_symlink(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(link):
                MVM_file_link(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(gethostname):
                GET_REG(cur_op, 0).s = MVM_io_get_hostname(tc);
                cur_op += 2;
                goto NEXT;
            OP(exreturnafterunwind): {
                MVMObject *ex = GET_REG(cur_op, 0).o;
                MVM_exception_returnafterunwind(tc, ex);
                cur_op += 2;
                goto NEXT;
            }
            OP(vmeventsubscribe): {
                MVMObject *queue  = GET_REG(cur_op, 0).o;
                MVMObject *config = GET_REG(cur_op, 2).o;
                MVM_vm_event_subscription_configure(tc, queue, config);
                cur_op += 4;
                goto NEXT;
            }
            OP(continuationreset): {
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *tag  = GET_REG(cur_op, 2).o;
                MVMObject   *code = GET_REG(cur_op, 4).o;
                cur_op += 6;
                MVM_continuation_reset(tc, tag, code, res);
                goto NEXT;
            }
            OP(continuationcontrol): {
                MVMRegister *res     = &GET_REG(cur_op, 0);
                MVMint64     protect = GET_REG(cur_op, 2).i64;
                MVMObject   *tag     = GET_REG(cur_op, 4).o;
                MVMObject   *code    = GET_REG(cur_op, 6).o;
                cur_op += 8;
                MVM_continuation_control(tc, protect, tag, code, res);
                goto NEXT;
            }
            OP(continuationinvoke): {
                MVMRegister *res  = &GET_REG(cur_op, 0);
                MVMObject   *cont = GET_REG(cur_op, 2).o;
                MVMObject   *code = GET_REG(cur_op, 4).o;
                cur_op += 6;
                MVM_continuation_invoke(tc, (MVMContinuation *)cont, code, res);
                goto NEXT;
            }
            OP(randscale_n):
                GET_REG(cur_op, 0).n64 = MVM_proc_rand_n(tc) * GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(uniisblock):
                GET_REG(cur_op, 0).i64 = (MVMint64)MVM_unicode_is_in_block(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).s);
                cur_op += 8;
                goto NEXT;
            OP(assertparamcheck): {
                MVMint64 ok = GET_REG(cur_op, 0).i64;
                cur_op += 2;
                if (!ok)
                    MVM_args_bind_failed(tc);
                goto NEXT;
            }
            OP(hintfor): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = REPR(obj)->attr_funcs.hint_for(tc,
                    STABLE(obj), obj,
                    GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(paramnamesused): {
                MVMArgProcContext *ctx = &tc->cur_frame->params;
                if (ctx->callsite->num_pos != ctx->callsite->arg_count)
                    MVM_args_assert_nameds_used(tc, ctx);
                goto NEXT;
            }
            OP(getuniname): {
                GET_REG(cur_op, 0).s = MVM_unicode_get_name(tc, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            }
            OP(getuniprop_int):
                GET_REG(cur_op, 0).i64 = MVM_unicode_codepoint_get_property_int(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(getuniprop_bool):
                GET_REG(cur_op, 0).i64 = MVM_unicode_codepoint_get_property_bool(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(getuniprop_str):
                GET_REG(cur_op, 0).s = MVM_unicode_codepoint_get_property_str(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(matchuniprop):
                GET_REG(cur_op, 0).i64 = MVM_unicode_codepoint_has_property_value(tc,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(nativecallbuild):
                GET_REG(cur_op, 0).i64 = MVM_nativecall_build(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).s, GET_REG(cur_op, 8).s,
                    GET_REG(cur_op, 10).o, GET_REG(cur_op, 12).o);
                cur_op += 14;
                goto NEXT;
            OP(nativecallinvoke):
                GET_REG(cur_op, 0).o = MVM_nativecall_invoke(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            OP(nativecallrefresh):
                MVM_nativecall_refresh(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(threadrun):
                MVM_thread_run(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(threadid):
                GET_REG(cur_op, 0).i64 = MVM_thread_id(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(threadyield):
                MVM_thread_yield(tc);
                goto NEXT;
            OP(currentthread):
                GET_REG(cur_op, 0).o = MVM_thread_current(tc);
                cur_op += 2;
                goto NEXT;
            OP(lock):
                MVM_reentrantmutex_lock_checked(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(unlock):
                MVM_reentrantmutex_unlock_checked(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(semacquire): {
                MVMObject *sem = GET_REG(cur_op, 0).o;
                if (REPR(sem)->ID == MVM_REPR_ID_Semaphore && IS_CONCRETE(sem))
                    MVM_semaphore_acquire(tc, (MVMSemaphore *)sem);
                else
                    MVM_exception_throw_adhoc(tc,
                        "semacquire requires a concrete object with REPR Semaphore");
                cur_op += 2;
                goto NEXT;
            }
            OP(semtryacquire): {
                MVMObject *sem = GET_REG(cur_op, 2).o;
                if (REPR(sem)->ID == MVM_REPR_ID_Semaphore && IS_CONCRETE(sem))
                    GET_REG(cur_op, 0).i64 = MVM_semaphore_tryacquire(tc,
                        (MVMSemaphore *)sem);
                else
                    MVM_exception_throw_adhoc(tc,
                        "semtryacquire requires a concrete object with REPR Semaphore");
                cur_op += 4;
                goto NEXT;
            }
            OP(semrelease): {
                MVMObject *sem = GET_REG(cur_op, 0).o;
                if (REPR(sem)->ID == MVM_REPR_ID_Semaphore && IS_CONCRETE(sem))
                    MVM_semaphore_release(tc, (MVMSemaphore *)sem);
                else
                    MVM_exception_throw_adhoc(tc,
                        "semrelease requires a concrete object with REPR Semaphore");
                cur_op += 2;
                goto NEXT;
            }
            OP(getlockcondvar): {
                MVMObject *lock = GET_REG(cur_op, 2).o;
                if (REPR(lock)->ID == MVM_REPR_ID_ReentrantMutex && IS_CONCRETE(lock))
                    GET_REG(cur_op, 0).o = MVM_conditionvariable_from_lock(tc,
                        (MVMReentrantMutex *)lock, GET_REG(cur_op, 4).o);
                else
                    MVM_exception_throw_adhoc(tc,
                        "getlockcondvar requires a concrete object with REPR ReentrantMutex");
                cur_op += 6;
                goto NEXT;
            }
            OP(condwait): {
                MVMObject *cv = GET_REG(cur_op, 0).o;
                if (REPR(cv)->ID == MVM_REPR_ID_ConditionVariable && IS_CONCRETE(cv))
                    MVM_conditionvariable_wait(tc, (MVMConditionVariable *)cv);
                else
                    MVM_exception_throw_adhoc(tc,
                        "condwait requires a concrete object with REPR ConditionVariable");
                cur_op += 2;
                goto NEXT;
            }
            OP(condsignalone): {
                MVMObject *cv = GET_REG(cur_op, 0).o;
                if (REPR(cv)->ID == MVM_REPR_ID_ConditionVariable && IS_CONCRETE(cv))
                    MVM_conditionvariable_signal_one(tc, (MVMConditionVariable *)cv);
                else
                    MVM_exception_throw_adhoc(tc,
                        "condsignalone requires a concrete object with REPR ConditionVariable");
                cur_op += 2;
                goto NEXT;
            }
            OP(condsignalall): {
                MVMObject *cv = GET_REG(cur_op, 0).o;
                if (REPR(cv)->ID == MVM_REPR_ID_ConditionVariable && IS_CONCRETE(cv))
                    MVM_conditionvariable_signal_all(tc, (MVMConditionVariable *)cv);
                else
                    MVM_exception_throw_adhoc(tc,
                        "condsignalall requires a concrete object with REPR ConditionVariable");
                cur_op += 2;
                goto NEXT;
            }
            OP(queuepoll): {
                MVMObject *queue = GET_REG(cur_op, 2).o;
                if (REPR(queue)->ID == MVM_REPR_ID_ConcBlockingQueue && IS_CONCRETE(queue))
                    GET_REG(cur_op, 0).o = MVM_concblockingqueue_poll(tc,
                        (MVMConcBlockingQueue *)queue);
                else
                    MVM_exception_throw_adhoc(tc,
                        "queuepoll requires a concrete object with REPR ConcBlockingQueue");
                cur_op += 4;
                goto NEXT;
            }
            OP(setmultispec): {
                MVMObject *obj        = GET_REG(cur_op, 0).o;
                MVMObject *ch         = GET_REG(cur_op, 2).o;
                MVMString *valid_attr = GET_REG(cur_op, 4).s;
                MVMString *cache_attr = GET_REG(cur_op, 6).s;
                MVMSTable *st         = STABLE(obj);
                MVMInvocationSpec *is = st->invocation_spec;
                if (!is)
                    MVM_exception_throw_adhoc(tc,
                        "Can only use setmultispec after setinvokespec");
                MVM_ASSIGN_REF(tc, &(st->header), is->md_class_handle, ch);
                MVM_ASSIGN_REF(tc, &(st->header), is->md_valid_attr_name, valid_attr);
                MVM_ASSIGN_REF(tc, &(st->header), is->md_cache_attr_name, cache_attr);
                is->md_valid_hint = REPR(ch)->attr_funcs.hint_for(tc, STABLE(ch), ch,
                    valid_attr);
                is->md_cache_hint = REPR(ch)->attr_funcs.hint_for(tc, STABLE(ch), ch,
                    cache_attr);
                cur_op += 8;
                goto NEXT;
            }
            OP(ctxouterskipthunks): {
                MVMObject *ctx = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(ctx) || REPR(ctx)->ID != MVM_REPR_ID_MVMContext)
                    MVM_exception_throw_adhoc(tc, "ctxouter needs an MVMContext");
                GET_REG(cur_op, 0).o = MVM_context_apply_traversal(tc, (MVMContext *)ctx,
                        MVM_CTX_TRAV_OUTER_SKIP_THUNKS);
                cur_op += 4;
                goto NEXT;
            }
            OP(ctxcallerskipthunks): {
                MVMObject *ctx = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(ctx) || REPR(ctx)->ID != MVM_REPR_ID_MVMContext)
                    MVM_exception_throw_adhoc(tc, "ctxcallerskipthunks needs an MVMContext");
                GET_REG(cur_op, 0).o = MVM_context_apply_traversal(tc, (MVMContext *)ctx,
                        MVM_CTX_TRAV_CALLER_SKIP_THUNKS);
                cur_op += 4;
                goto NEXT;
            }
            OP(timer):
                GET_REG(cur_op, 0).o = MVM_io_timer_create(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).i64,
                    GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(cancel):
                MVM_io_eventloop_cancel_work(tc, GET_REG(cur_op, 0).o, NULL, NULL);
                cur_op += 2;
                goto NEXT;
            OP(signal):
                GET_REG(cur_op, 0).o = MVM_io_signal_handle(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).o);
                cur_op += 10;
                goto NEXT;
            OP(watchfile):
                GET_REG(cur_op, 0).o = MVM_io_file_watch(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s, GET_REG(cur_op, 8).o);
                cur_op += 10;
                goto NEXT;
            OP(asyncconnect):
                GET_REG(cur_op, 0).o = MVM_io_socket_connect_async(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(asynclisten):
                GET_REG(cur_op, 0).o = MVM_io_socket_listen_async(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    GET_REG(cur_op, 8).i64, (MVMint32)GET_REG(cur_op, 10).i64, GET_REG(cur_op, 12).o);
                cur_op += 14;
                goto NEXT;
            OP(asyncwritebytes):
                GET_REG(cur_op, 0).o = MVM_io_write_bytes_async(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o,
                    GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(asyncreadbytes):
                GET_REG(cur_op, 0).o = MVM_io_read_bytes_async(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o,
                    GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(getlexstatic_o): {
                MVMRegister *found = MVM_frame_find_lexical_by_name(tc,
                    GET_REG(cur_op, 2).s, MVM_reg_obj);
                if (found) {
                    GET_REG(cur_op, 0).o = found->o;
                    if (MVM_spesh_log_is_logging(tc))
                        MVM_spesh_log_static(tc, found->o);
                }
                else {
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(getlexperinvtype_o): {
                MVMRegister *found = MVM_frame_find_lexical_by_name(tc,
                    GET_REG(cur_op, 2).s, MVM_reg_obj);
                if (found) {
                    GET_REG(cur_op, 0).o = found->o;
                    if (MVM_spesh_log_is_logging(tc))
                        MVM_spesh_log_type(tc, found->o);
                }
                else {
                    GET_REG(cur_op, 0).o = tc->instance->VMNull;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(execname):
                GET_REG(cur_op, 0).s = MVM_executable_name(tc);
                cur_op += 2;
                goto NEXT;
            OP(const_i64_16):
                GET_REG(cur_op, 0).i64 = GET_I16(cur_op, 2);
                cur_op += 4;
                goto NEXT;
            OP(const_i64_32):
                GET_REG(cur_op, 0).i64 = GET_I32(cur_op, 2);
                cur_op += 6;
                goto NEXT;
            OP(isnonnull): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).i64 = !MVM_is_null(tc, obj);
                cur_op += 4;
                goto NEXT;
            }
            OP(param_rn2_i): {
                MVMArgInfo param = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists)
                    GET_REG(cur_op, 0).i64 = param.arg.i64;
                else
                    GET_REG(cur_op, 0).i64 = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_REQUIRED).arg.i64;
                cur_op += 10;
                goto NEXT;
            }
            OP(param_rn2_n): {
                MVMArgInfo param = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists)
                    GET_REG(cur_op, 0).n64 = param.arg.n64;
                else
                    GET_REG(cur_op, 0).n64 = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_REQUIRED).arg.n64;
                cur_op += 10;
                goto NEXT;
            }
            OP(param_rn2_s): {
                MVMArgInfo param = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists)
                    GET_REG(cur_op, 0).s = param.arg.s;
                else
                    GET_REG(cur_op, 0).s = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_REQUIRED).arg.s;
                cur_op += 10;
                goto NEXT;
            }
            OP(param_rn2_o): {
                MVMArgInfo param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_REQUIRED);
                GET_REG(cur_op, 0).o = param.arg.o;
                cur_op += 10;
                goto NEXT;
            }
            OP(param_on2_i): {
                MVMArgInfo param = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).i64 = param.arg.i64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 10);
                }
                else {
                    cur_op += 14;
                }
                goto NEXT;
            }
            OP(param_on2_n): {
                MVMArgInfo param = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).n64 = param.arg.n64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 10);
                }
                else {
                    cur_op += 14;
                }
                goto NEXT;
            }
            OP(param_on2_s): {
                MVMArgInfo param = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).s = param.arg.s;
                    cur_op = bytecode_start + GET_UI32(cur_op, 10);
                }
                else {
                    cur_op += 14;
                }
                goto NEXT;
            }
            OP(param_on2_o): {
                MVMArgInfo param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).o = param.arg.o;
                    cur_op = bytecode_start + GET_UI32(cur_op, 10);
                }
                else {
                    cur_op += 14;
                }
                goto NEXT;
            }
            OP(osrpoint):
                if (MVM_spesh_log_is_logging(tc))
                    MVM_spesh_log_osr(tc);
                MVM_spesh_osr_poll_for_result(tc);
                goto NEXT;
            OP(nativecallcast):
                GET_REG(cur_op, 0).o = MVM_nativecall_cast(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            OP(spawnprocasync):
                GET_REG(cur_op, 0).o = MVM_proc_spawn_async(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    GET_REG(cur_op, 8).o, GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(killprocasync):
                MVM_proc_kill_async(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(startprofile):
                MVM_profile_start(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(endprofile):
                GET_REG(cur_op, 0).o = MVM_profile_end(tc);
                cur_op += 2;
                goto NEXT;
            OP(objectid):
                GET_REG(cur_op, 0).i64 = (MVMint64)MVM_gc_object_id(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(settypefinalize):
                MVM_gc_finalize_set(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(force_gc):
                MVM_gc_enter_from_allocator(tc);
                goto NEXT;
            OP(nativecallglobal):
                GET_REG(cur_op, 0).o = MVM_nativecall_global(tc, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o);
                cur_op += 10;
                goto NEXT;
            OP(setparameterizer):
                MVM_6model_parametric_setup(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(parameterizetype): {
                MVMObject   *type   = GET_REG(cur_op, 2).o;
                MVMObject   *params = GET_REG(cur_op, 4).o;
                MVMRegister *result = &(GET_REG(cur_op, 0));
                cur_op += 6;
                MVM_6model_parametric_parameterize(tc, type, params, result);
                goto NEXT;
            }
            OP(typeparameterized):
                GET_REG(cur_op, 0).o = MVM_6model_parametric_type_parameterized(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(typeparameters):
                GET_REG(cur_op, 0).o = MVM_6model_parametric_type_parameters(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(typeparameterat):
                GET_REG(cur_op, 0).o = MVM_6model_parametric_type_parameter_at(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(readlink):
                GET_REG(cur_op, 0).s = MVM_file_readlink(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(lstat):
                GET_REG(cur_op, 0).i64 = MVM_file_stat(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, 1);
                cur_op += 6;
                goto NEXT;
            OP(iscont_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_iscont_i(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(iscont_n):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_iscont_n(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(iscont_s):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_iscont_s(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(assign_i): {
                MVMObject *cont  = GET_REG(cur_op, 0).o;
                MVMint64   value = GET_REG(cur_op, 2).i64;
                cur_op += 4;
                MVM_6model_container_assign_i(tc, cont, value);
                goto NEXT;
            }
            OP(assign_n): {
                MVMObject *cont  = GET_REG(cur_op, 0).o;
                MVMnum64   value = GET_REG(cur_op, 2).n64;
                cur_op += 4;
                MVM_6model_container_assign_n(tc, cont, value);
                goto NEXT;
            }
            OP(assign_s): {
                MVMObject *cont  = GET_REG(cur_op, 0).o;
                MVMString *value = GET_REG(cur_op, 2).s;
                cur_op += 4;
                MVM_6model_container_assign_s(tc, cont, value);
                goto NEXT;
            }
            OP(decont_i): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_6model_container_decont_i(tc, obj, r);
                goto NEXT;
            }
            OP(decont_n): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_6model_container_decont_n(tc, obj, r);
                goto NEXT;
            }
            OP(decont_s): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_6model_container_decont_s(tc, obj, r);
                goto NEXT;
            }
            OP(getrusage):
                MVM_proc_getrusage(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(threadlockcount):
                GET_REG(cur_op, 0).i64 = MVM_thread_lock_count(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(getlexref_i):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_i(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_n):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_n(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_s):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_s(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_ni):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_name_i(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_nn):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_name_n(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_ns):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_name_s(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)));
                cur_op += 6;
                goto NEXT;
            OP(atposref_i):
                GET_REG(cur_op, 0).o = MVM_nativeref_pos_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(atposref_n):
                GET_REG(cur_op, 0).o = MVM_nativeref_pos_n(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(atposref_s):
                GET_REG(cur_op, 0).o = MVM_nativeref_pos_s(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(getattrref_i):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)));
                cur_op += 12;
                goto NEXT;
            OP(getattrref_n):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_n(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)));
                cur_op += 12;
                goto NEXT;
            OP(getattrref_s):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_s(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)));
                cur_op += 12;
                goto NEXT;
            OP(getattrsref_i):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 6).s);
                cur_op += 8;
                goto NEXT;
            OP(getattrsref_n):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_n(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 6).s);
                cur_op += 8;
                goto NEXT;
            OP(getattrsref_s):
                GET_REG(cur_op, 0).o = MVM_nativeref_attr_s(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    GET_REG(cur_op, 6).s);
                cur_op += 8;
                goto NEXT;
            OP(nativecallsizeof):
                GET_REG(cur_op, 0).i64 = MVM_nativecall_sizeof(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(encodenorm):
                MVM_exception_throw_adhoc(tc, "NYI");
            OP(normalizecodes):
                MVM_unicode_normalize_codepoints(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 4).o,
                    MVM_unicode_normalizer_form(tc, GET_REG(cur_op, 2).i64));
                cur_op += 6;
                goto NEXT;
            OP(strfromcodes):
                GET_REG(cur_op, 0).s = MVM_unicode_codepoints_to_nfg_string(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(strtocodes):
                MVM_unicode_string_to_codepoints(tc, GET_REG(cur_op, 0).s,
                    MVM_unicode_normalizer_form(tc, GET_REG(cur_op, 2).i64),
                    GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(getcodelocation):
                GET_REG(cur_op, 0).o = MVM_code_location(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(eqatim_s):
                GET_REG(cur_op, 0).i64 = MVM_string_equal_at_ignore_mark(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(ordbaseat):
                GET_REG(cur_op, 0).i64 = MVM_string_ord_basechar_at(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(neverrepossess):
                MVM_6model_never_repossess(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(scdisclaim):
                MVM_sc_disclaim(tc, (MVMSerializationContext *)GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(atpos2d_i):
                GET_REG(cur_op, 0).i64 = MVM_repr_at_pos_2d_i(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(atpos2d_n):
                GET_REG(cur_op, 0).n64 = MVM_repr_at_pos_2d_n(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(atpos2d_s):
                GET_REG(cur_op, 0).s = MVM_repr_at_pos_2d_s(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(atpos2d_o):
                GET_REG(cur_op, 0).o = MVM_repr_at_pos_2d_o(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(atpos3d_i):
                GET_REG(cur_op, 0).i64 = MVM_repr_at_pos_3d_i(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(atpos3d_n):
                GET_REG(cur_op, 0).n64 = MVM_repr_at_pos_3d_n(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(atpos3d_s):
                GET_REG(cur_op, 0).s = MVM_repr_at_pos_3d_s(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(atpos3d_o):
                GET_REG(cur_op, 0).o = MVM_repr_at_pos_3d_o(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(atposnd_i):
                GET_REG(cur_op, 0).i64 = MVM_repr_at_pos_multidim_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(atposnd_n):
                GET_REG(cur_op, 0).n64 = MVM_repr_at_pos_multidim_n(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(atposnd_s):
                GET_REG(cur_op, 0).s = MVM_repr_at_pos_multidim_s(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(atposnd_o):
                GET_REG(cur_op, 0).o = MVM_repr_at_pos_multidim_o(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(bindpos2d_i):
                MVM_repr_bind_pos_2d_i(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(bindpos2d_n):
                MVM_repr_bind_pos_2d_n(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).n64);
                cur_op += 8;
                goto NEXT;
            OP(bindpos2d_s):
                MVM_repr_bind_pos_2d_s(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).s);
                cur_op += 8;
                goto NEXT;
            OP(bindpos2d_o):
                MVM_repr_bind_pos_2d_o(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            OP(bindpos3d_i):
                MVM_repr_bind_pos_3d_i(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(bindpos3d_n):
                MVM_repr_bind_pos_3d_n(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).n64);
                cur_op += 10;
                goto NEXT;
            OP(bindpos3d_s):
                MVM_repr_bind_pos_3d_s(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).s);
                cur_op += 10;
                goto NEXT;
            OP(bindpos3d_o):
                MVM_repr_bind_pos_3d_o(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).o);
                cur_op += 10;
                goto NEXT;
            OP(bindposnd_i):
                MVM_repr_bind_pos_multidim_i(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(bindposnd_n):
                MVM_repr_bind_pos_multidim_n(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).n64);
                cur_op += 6;
                goto NEXT;
            OP(bindposnd_s):
                MVM_repr_bind_pos_multidim_s(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            OP(bindposnd_o):
                MVM_repr_bind_pos_multidim_o(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(dimensions):
                GET_REG(cur_op, 0).o = MVM_repr_dimensions(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(setdimensions):
                MVM_repr_set_dimensions(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(numdimensions):
                GET_REG(cur_op, 0).i64 = MVM_repr_num_dimensions(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(ctxcode): {
                MVMObject *this_ctx = GET_REG(cur_op, 2).o;
                if (IS_CONCRETE(this_ctx) && REPR(this_ctx)->ID == MVM_REPR_ID_MVMContext) {
                    GET_REG(cur_op, 0).o = MVM_context_get_code(tc, (MVMContext *)this_ctx);
                    cur_op += 4;
                }
                else {
                    MVM_exception_throw_adhoc(tc, "ctxcode needs an MVMContext");
                }
                goto NEXT;
            }
            OP(isrwcont): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMint64 is_rw = 0;
                if (!MVM_is_null(tc, obj)) {
                    const MVMContainerSpec *cs = STABLE(obj)->container_spec;
                    is_rw = cs && cs->can_store(tc, obj);
                }
                GET_REG(cur_op, 0).i64 = is_rw;
                cur_op += 4;
                goto NEXT;
            }
            OP(fc):
                GET_REG(cur_op, 0).s = MVM_string_fc(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(encoderep):
                GET_REG(cur_op, 0).o = MVM_string_encode_to_buf(tc, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).s, GET_REG(cur_op, 8).o, GET_REG(cur_op, 6).s);
                cur_op += 10;
                goto NEXT;
            OP(istty_fh):
                GET_REG(cur_op, 0).i64 = MVM_io_is_tty(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(multidimref_i):
                GET_REG(cur_op, 0).o = MVM_nativeref_multidim_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(multidimref_n):
                GET_REG(cur_op, 0).o = MVM_nativeref_multidim_n(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(multidimref_s):
                GET_REG(cur_op, 0).o = MVM_nativeref_multidim_s(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(fileno_fh):
                GET_REG(cur_op, 0).i64 = MVM_io_fileno(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(asyncudp):
                GET_REG(cur_op, 0).o = MVM_io_socket_udp_async(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).i64,
                    GET_REG(cur_op, 12).o);
                cur_op += 14;
                goto NEXT;
            OP(asyncwritebytesto):
                GET_REG(cur_op, 0).o = MVM_io_write_bytes_to_async(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).o, GET_REG(cur_op, 8).o,
                    GET_REG(cur_op, 10).o, GET_REG(cur_op, 12).s, GET_REG(cur_op, 14).i64);
                cur_op += 16;
                goto NEXT;
            OP(objprimbits): {
                MVMObject *type = GET_REG(cur_op, 2).o;
                if (type) {
                    const MVMStorageSpec *ss = REPR(type)->get_storage_spec(tc, STABLE(type));
                    GET_REG(cur_op, 0).i64 = ss->boxed_primitive ? ss->bits : 0;
                }
                else {
                    GET_REG(cur_op, 0).i64 = 0;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(objprimunsigned): {
                MVMObject *type = GET_REG(cur_op, 2).o;
                if (type) {
                    const MVMStorageSpec *ss = REPR(type)->get_storage_spec(tc, STABLE(type));
                    GET_REG(cur_op, 0).i64 = ss->boxed_primitive == 1 ? ss->is_unsigned : 0;
                }
                else {
                    GET_REG(cur_op, 0).i64 = 0;
                }
                cur_op += 4;
                goto NEXT;
            }
            OP(getlexref_i32):
            OP(getlexref_i16):
            OP(getlexref_i8):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_i(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(getlexref_n32):
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_n(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(box_u): {
                MVM_box_uint(tc, GET_REG(cur_op, 2).u64, GET_REG(cur_op, 4).o,
                            &GET_REG(cur_op, 0));
                cur_op += 6;
                goto NEXT;
            }
            OP(unbox_u): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                if (!IS_CONCRETE(obj))
                    MVM_exception_throw_adhoc(tc, "Cannot unbox a type object");
                GET_REG(cur_op, 0).u64 = REPR(obj)->box_funcs.get_uint(tc,
                    STABLE(obj), obj, OBJECT_BODY(obj));
                cur_op += 4;
                goto NEXT;
            }
            OP(coerce_iu):
                GET_REG(cur_op, 0).u64 = (MVMuint64)GET_REG(cur_op, 2).i64;
                cur_op += 4;
                goto NEXT;
            OP(coerce_ui):
                GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).u64;
                cur_op += 4;
                goto NEXT;
            OP(coerce_nu):
                GET_REG(cur_op, 0).u64 = (MVMuint64)GET_REG(cur_op, 2).n64;
                cur_op += 4;
                goto NEXT;
            OP(coerce_un):
                GET_REG(cur_op, 0).n64 = (MVMnum64)GET_REG(cur_op, 2).u64;
                cur_op += 4;
                goto NEXT;
            OP(decont_u): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                MVM_6model_container_decont_u(tc, obj, r);
                goto NEXT;
            }
            OP(getlexref_u):
            OP(getlexref_u32):
            OP(getlexref_u16):
            OP(getlexref_u8):
                /* XXX Cheat should have a _u here. */
                GET_REG(cur_op, 0).o = MVM_nativeref_lex_i(tc,
                    GET_UI16(cur_op, 4), GET_UI16(cur_op, 2));
                cur_op += 6;
                goto NEXT;
            OP(param_rp_u):
                GET_REG(cur_op, 0).u64 = MVM_args_get_required_pos_uint(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                cur_op += 4;
                goto NEXT;
            OP(param_op_u): {
                MVMArgInfo param = MVM_args_get_optional_pos_uint(tc, &tc->cur_frame->params,
                    GET_UI16(cur_op, 2));
                if (param.exists) {
                    GET_REG(cur_op, 0).u64 = param.arg.u64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 4);
                }
                else {
                    cur_op += 8;
                }
                goto NEXT;
            }
            OP(param_rn_u):
                GET_REG(cur_op, 0).u64 = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_REQUIRED).arg.u64;
                cur_op += 6;
                goto NEXT;
            OP(param_on_u):{
                MVMArgInfo param = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).u64 = param.arg.u64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 6);
                }
                else {
                    cur_op += 10;
                }
                goto NEXT;
            }
            OP(param_rn2_u): {
                MVMArgInfo param = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (param.exists)
                    GET_REG(cur_op, 0).u64 = param.arg.u64;
                else
                    GET_REG(cur_op, 0).u64 = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_REQUIRED).arg.u64;
                cur_op += 10;
                goto NEXT;
            }
            OP(param_on2_u): {
                MVMArgInfo param = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_ARG_OPTIONAL);
                if (!param.exists)
                    param = MVM_args_get_named_uint(tc, &tc->cur_frame->params,
                        MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)), MVM_ARG_OPTIONAL);
                if (param.exists) {
                    GET_REG(cur_op, 0).u64 = param.arg.u64;
                    cur_op = bytecode_start + GET_UI32(cur_op, 10);
                }
                else {
                    cur_op += 14;
                }
                goto NEXT;
            }
            OP(stat_time):
                GET_REG(cur_op, 0).n64 = MVM_file_time(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, 0);
                cur_op += 6;
                goto NEXT;
            OP(lstat_time):
                GET_REG(cur_op, 0).n64 = MVM_file_time(tc, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, 1);
                cur_op += 6;
                goto NEXT;
            OP(setdebugtypename):
                MVM_6model_set_debug_name(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(loadbytecodebuffer): {
                /* This op will end up returning into the runloop to run
                 * deserialization and load code, so make sure we're done
                 * processing this op really. */
                MVMObject *buffer = GET_REG(cur_op, 0).o;
                cur_op += 2;

                /* Set up return (really continuation after load) address
                 * and enter bytecode loading process. */
                tc->cur_frame->return_address = cur_op;
                MVM_load_bytecode_buffer(tc, buffer);
                goto NEXT;
            }
            OP(buffertocu): {
                /* This op will end up returning into the runloop to run
                 * deserialization and load code, so make sure we're done
                 * processing this op really. */
                MVMRegister *result_reg = &GET_REG(cur_op, 0);
                MVMObject *buffer = GET_REG(cur_op, 2).o;
                cur_op += 4;

                /* Set up return (really continuation after load) address
                 * and enter bytecode loading process. */
                tc->cur_frame->return_address = cur_op;
                MVM_load_bytecode_buffer_to_cu(tc, buffer, result_reg);
                goto NEXT;
            }
            OP(loadbytecodefh): {
                /* This op will end up returning into the runloop to run
                 * deserialization and load code, so make sure we're done
                 * processing this op really. */
                MVMObject *file = GET_REG(cur_op, 0).o;
                MVMString *filename = GET_REG(cur_op, 2).s;
                cur_op += 4;

                /* Set up return (really continuation after load) address
                 * and enter bytecode loading process. */
                tc->cur_frame->return_address = cur_op;
                MVM_load_bytecode_fh(tc, file, filename);
                goto NEXT;
            }
            OP(throwpayloadlex): {
                MVMRegister *rr      = &GET_REG(cur_op, 0);
                MVMuint32    cat     = (MVMuint32)MVM_BC_get_I64(cur_op, 2);
                MVMObject   *payload = GET_REG(cur_op, 10).o;
                cur_op += 12;
                MVM_exception_throwpayload(tc, MVM_EX_THROW_LEX, cat, payload, rr);
                goto NEXT;
            }
            OP(throwpayloadlexcaller): {
                MVMRegister *rr      = &GET_REG(cur_op, 0);
                MVMuint32    cat     = (MVMuint32)MVM_BC_get_I64(cur_op, 2);
                MVMObject   *payload = GET_REG(cur_op, 10).o;
                cur_op += 12;
                MVM_exception_throwpayload(tc, MVM_EX_THROW_LEX_CALLER, cat, payload, rr);
                goto NEXT;
            }
            OP(lastexpayload):
                GET_REG(cur_op, 0).o = tc->last_payload;
                cur_op += 2;
                goto NEXT;
            OP(cancelnotify):
                MVM_io_eventloop_cancel_work(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(decoderconfigure): {
                MVMObject *decoder = GET_REG(cur_op, 0).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decoderconfigure");
                MVM_decoder_configure(tc, (MVMDecoder *)decoder,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(decodersetlineseps): {
                MVMObject *decoder = GET_REG(cur_op, 0).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodersetlineseps");
                MVM_decoder_set_separators(tc, (MVMDecoder *)decoder, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(decoderaddbytes): {
                MVMObject *decoder = GET_REG(cur_op, 0).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decoderaddbytes");
                MVM_decoder_add_bytes(tc, (MVMDecoder *)decoder, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(decodertakechars): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakechars");
                GET_REG(cur_op, 0).s = MVM_decoder_take_chars(tc, (MVMDecoder *)decoder,
                    GET_REG(cur_op, 4).i64, 0);
                cur_op += 6;
                goto NEXT;
            }
            OP(decodertakeallchars): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakeallchars");
                GET_REG(cur_op, 0).s = MVM_decoder_take_all_chars(tc, (MVMDecoder *)decoder);
                cur_op += 4;
                goto NEXT;
            }
            OP(decodertakeavailablechars): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakeavailablechars");
                GET_REG(cur_op, 0).s = MVM_decoder_take_available_chars(tc, (MVMDecoder *)decoder);
                cur_op += 4;
                goto NEXT;
            }
            OP(decodertakeline): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakeline");
                GET_REG(cur_op, 0).s = MVM_decoder_take_line(tc, (MVMDecoder *)decoder,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(decoderbytesavailable): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decoderbytesavailable");
                GET_REG(cur_op, 0).i64 = MVM_decoder_bytes_available(tc, (MVMDecoder *)decoder);
                cur_op += 4;
                goto NEXT;
            }
            OP(decodertakebytes): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakebytes");
                GET_REG(cur_op, 0).o = MVM_decoder_take_bytes(tc, (MVMDecoder *)decoder,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(decoderempty): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decoderempty");
                GET_REG(cur_op, 0).i64 = MVM_decoder_empty(tc, (MVMDecoder *)decoder);
                cur_op += 4;
                goto NEXT;
            }
            OP(indexingoptimized):
                GET_REG(cur_op, 0).s = MVM_string_indexing_optimized(tc, GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(captureinnerlex): {
                MVMObject *code = (MVMObject *)GET_REG(cur_op, 0).o;
                CHECK_CONC(code);
                MVM_frame_capture_inner(tc, code);
                cur_op += 2;
                goto NEXT;
            }
            OP(unicmp_s):
                GET_REG(cur_op, 0).i64 = MVM_unicode_string_compare(tc,
                    GET_REG(cur_op,  2).s,   GET_REG(cur_op, 4).s,
                    GET_REG(cur_op,  6).i64, GET_REG(cur_op, 8).i64,
                    GET_REG(cur_op, 10).i64);
                cur_op += 12;
                goto NEXT;
            OP(setdispatcherfor): {
                MVMObject *disp_for = GET_REG(cur_op, 2).o;
                tc->cur_dispatcher = GET_REG(cur_op, 0).o;
                tc->cur_dispatcher_for = REPR(disp_for)->ID == MVM_REPR_ID_MVMCode
                    ? disp_for
                    : MVM_frame_find_invokee(tc, disp_for, NULL);
                cur_op += 4;
                goto NEXT;
            }
            OP(strfromname):
                GET_REG(cur_op, 0).s = MVM_unicode_string_from_name(tc,
                    GET_REG(cur_op, 2).s);
                cur_op += 4;
                goto NEXT;
            OP(indexic_s):
                GET_REG(cur_op, 0).i64 = MVM_string_index_ignore_case(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(getport_sk):
                GET_REG(cur_op, 0).i64 = MVM_io_getport(tc, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(cpucores):
                GET_REG(cur_op, 0).i64 = MVM_platform_cpu_count();
                cur_op += 2;
                goto NEXT;
            OP(eqaticim_s):
                GET_REG(cur_op, 0).i64 = MVM_string_equal_at_ignore_case_ignore_mark(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(indexicim_s):
                GET_REG(cur_op, 0).i64 = MVM_string_index_ignore_case_ignore_mark(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(decodertakecharseof): {
                MVMObject *decoder = GET_REG(cur_op, 2).o;
                MVM_decoder_ensure_decoder(tc, decoder, "decodertakecharseof");
                GET_REG(cur_op, 0).s = MVM_decoder_take_chars(tc, (MVMDecoder *)decoder,
                    GET_REG(cur_op, 4).i64, 1);
                cur_op += 6;
                goto NEXT;
            }
            OP(indexim_s):
                GET_REG(cur_op, 0).i64 = MVM_string_index_ignore_mark(tc,
                    GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(cas_o): {
                MVMRegister *result = &GET_REG(cur_op, 0);
                MVMObject *target = GET_REG(cur_op, 2).o;
                MVMObject *expected = GET_REG(cur_op, 4).o;
                MVMObject *value = GET_REG(cur_op, 6).o;
                cur_op += 8;
                MVM_6model_container_cas(tc, target, expected, value, result);
                goto NEXT;
            }
            OP(cas_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_cas_i(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64,
                    GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(atomicinc_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_atomic_inc(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(atomicdec_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_atomic_dec(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(atomicadd_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_atomic_add(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                cur_op += 6;
                goto NEXT;
            OP(atomicload_o):
                GET_REG(cur_op, 0).o = MVM_6model_container_atomic_load(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(atomicload_i):
                GET_REG(cur_op, 0).i64 = MVM_6model_container_atomic_load_i(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(atomicstore_o): {
                MVMObject *target = GET_REG(cur_op, 0).o;
                MVMObject *value = GET_REG(cur_op, 2).o;
                cur_op += 4;
                MVM_6model_container_atomic_store(tc, target, value);
                goto NEXT;
            }
            OP(atomicstore_i):
                MVM_6model_container_atomic_store_i(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).i64);
                cur_op += 4;
                goto NEXT;
            OP(barrierfull):
                MVM_barrier();
                goto NEXT;
            OP(coveragecontrol): {
                MVMuint32 cc = (MVMuint32)GET_REG(cur_op, 0).i64;
                if (tc->instance->coverage_control && (cc == 0 || cc == 1))
                    tc->instance->coverage_control = cc + 1;
                cur_op += 2;
                goto NEXT;
            }
            OP(nativeinvoke_v):
                tc->cur_frame->return_value = NULL;
                tc->cur_frame->return_type = MVM_RETURN_VOID;
                MVM_nativecall_invoke_jit(tc, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            OP(nativeinvoke_i):
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type = MVM_RETURN_INT;
                MVM_nativecall_invoke_jit(tc, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            OP(nativeinvoke_n):
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type = MVM_RETURN_NUM;
                MVM_nativecall_invoke_jit(tc, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            OP(nativeinvoke_s):
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type = MVM_RETURN_STR;
                MVM_nativecall_invoke_jit(tc, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            OP(nativeinvoke_o):
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type = MVM_RETURN_OBJ;
                MVM_nativecall_invoke_jit(tc, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            OP(getarg_i):
                GET_REG(cur_op, 0).i64 = tc->cur_frame->args[GET_REG(cur_op, 2).u16].i64;
                cur_op += 4;
                goto NEXT;
            OP(getarg_n):
                GET_REG(cur_op, 0).n64 = tc->cur_frame->args[GET_REG(cur_op, 2).u16].n64;
                cur_op += 4;
                goto NEXT;
            OP(getarg_s):
                GET_REG(cur_op, 0).s = tc->cur_frame->args[GET_REG(cur_op, 2).u16].s;
                cur_op += 4;
                goto NEXT;
            OP(getarg_o):
                GET_REG(cur_op, 0).o = tc->cur_frame->args[GET_REG(cur_op, 2).u16].o;
                cur_op += 4;
                goto NEXT;
            OP(coerce_II): {
                MVMObject *   const type = GET_REG(cur_op, 4).o;
                GET_REG(cur_op, 0).o = MVM_bigint_from_bigint(tc, type, GET_REG(cur_op, 2).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(encoderepconf):
                GET_REG(cur_op, 0).o = MVM_string_encode_to_buf_config(tc, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).s, GET_REG(cur_op, 8).o, GET_REG(cur_op, 6).s, GET_REG(cur_op, 10).i64);
                cur_op += 12;
                goto NEXT;
            OP(encodeconf):
                GET_REG(cur_op, 0).o = MVM_string_encode_to_buf_config(tc, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).o, NULL, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(decodeconf):
                GET_REG(cur_op, 0).s = MVM_string_decode_from_buf_config(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s, NULL, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            OP(decoderepconf):
                GET_REG(cur_op, 0).s = MVM_string_decode_from_buf_config(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).s, GET_REG(cur_op, 8).i64);
                cur_op += 10;
                goto NEXT;
            OP(getppid):
                GET_REG(cur_op, 0).i64 = MVM_proc_getppid(tc);
                cur_op += 2;
                goto NEXT;
            OP(getsignals):
                GET_REG(cur_op, 0).o = MVM_io_get_signals(tc);
                cur_op += 2;
                goto NEXT;
            OP(slice): {
                MVMObject *dest = MVM_repr_alloc_init(tc, GET_REG(cur_op, 2).o);
                MVMObject *src = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).o = dest;
                REPR(src)->pos_funcs.slice(tc, STABLE(src), src,
                    OBJECT_BODY(src), dest,
                    GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                cur_op += 8;
                goto NEXT;
            }
            OP(speshreg):
                MVM_spesh_plugin_register(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s,
                    GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            OP(speshresolve):
                MVM_spesh_plugin_resolve(tc, MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)),
                        &GET_REG(cur_op, 0), cur_op - 2, cur_op + 6, cur_callsite);
                goto NEXT;
            OP(speshguardtype):
                MVM_spesh_plugin_addguard_type(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(speshguardconcrete):
                MVM_spesh_plugin_addguard_concrete(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(speshguardtypeobj):
                MVM_spesh_plugin_addguard_typeobj(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(speshguardobj):
                MVM_spesh_plugin_addguard_obj(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(speshguardgetattr):
                GET_REG(cur_op, 0).o = MVM_spesh_plugin_addguard_getattr(tc,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 6)));
                cur_op += 10;
                goto NEXT;
            OP(atomicbindattr_o):
                MVM_repr_atomic_bind_attr_o(tc, GET_REG(cur_op, 0).o,
                    GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                    GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            OP(casattr_o):
                GET_REG(cur_op, 0).o = MVM_repr_casattr_o(tc, GET_REG(cur_op, 2).o,
                    GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                    GET_REG(cur_op, 8).o, GET_REG(cur_op, 10).o);
                cur_op += 12;
                goto NEXT;
            OP(atkey_u): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                REPR(obj)->ass_funcs.at_key(tc, STABLE(obj), obj, OBJECT_BODY(obj),
                    (MVMObject *)GET_REG(cur_op, 4).s, &GET_REG(cur_op, 0), MVM_reg_uint64);
                cur_op += 6;
                goto NEXT;
            }
            OP(coerce_us):
                GET_REG(cur_op, 0).s = MVM_coerce_u_s(tc, GET_REG(cur_op, 2).u64);
                cur_op += 4;
                goto NEXT;
            OP(decodelocaltime): {
                int i;
                MVMint64 decoded[9];
                MVMObject *result = MVM_repr_alloc_init(tc, tc->instance->boot_types.BOOTIntArray);

                GET_REG(cur_op, 0).o = result;

                MVM_platform_decodelocaltime(tc, GET_REG(cur_op, 2).i64, decoded);

                MVMROOT(tc, result, {
                    REPR(result)->pos_funcs.set_elems(tc, STABLE(result), result, OBJECT_BODY(result), 9);
                    for (i = 0; i < 9; i++) {
                        MVM_repr_bind_pos_i(tc, result, i, decoded[i]);
                    }
                });

                cur_op += 4;
                goto NEXT;
            }
            OP(speshguardnotobj):
                MVM_spesh_plugin_addguard_notobj(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(hllbool):
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).i64
                    ? cu->body.hll_config->true_value
                    : cu->body.hll_config->false_value;
                cur_op += 4;
                goto NEXT;
            OP(hllboolfor): {
                MVMString   *hll     = GET_REG(cur_op, 4).s;
                MVMHLLConfig *config = MVM_hll_get_config_for(tc, hll);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).i64
                    ? config->true_value
                    : config->false_value;
                cur_op += 6;
                goto NEXT;
            }
            OP(fork): {
                 GET_REG(cur_op, 0).i64 = MVM_proc_fork(tc);
                 cur_op += 2;
                 goto NEXT;
            }
            OP(writeint):
            OP(writeuint): {
                MVMObject*    const buf   = GET_REG(cur_op, 0).o;
                MVMint64      const off   = (MVMuint64)GET_REG(cur_op, 2).i64;
                MVMuint64           value = (MVMuint64)GET_REG(cur_op, 4).u64;
                MVMuint64     const flags = (MVMuint64)GET_REG(cur_op, 6).u64;
                unsigned char const size  = 1 << (flags >> 2);
                if (!IS_CONCRETE(buf))
                    MVM_exception_throw_adhoc(tc, "Cannot write to a %s type object", MVM_6model_get_debug_name(tc, buf));
                if ((flags & 3) == 3 || size > 8) {
                    MVM_exception_throw_adhoc(tc, "Invalid flags value for writeint");
                }
                if ((flags & 3) == MVM_SWITCHENDIAN) {
                    value = switch_endian(value, size);
                }
                REPR(buf)->pos_funcs.write_buf(tc, STABLE(buf), buf, OBJECT_BODY(buf),
                    (char*)&value
#if MVM_BIGENDIAN
                    + (8 - size)
#endif
                    , off, size);
                MVM_SC_WB_OBJ(tc, buf);
                cur_op += 8;
                goto NEXT;
            }
            OP(writenum): {
                MVMObject *buf  = GET_REG(cur_op, 0).o;
                MVMint64  off   = (MVMuint64)GET_REG(cur_op, 2).i64;
                MVMuint64 flags = (MVMuint64)GET_REG(cur_op, 6).u64;
                unsigned char size = 1 << (flags >> 2);
                MVMRegister byte;
                char i;
                if (!IS_CONCRETE(buf))
                    MVM_exception_throw_adhoc(tc, "Cannot write to a %s type object", MVM_6model_get_debug_name(tc, buf));
                switch (size) {
                    case 4: {
                        MVMnum32 num32 = (MVMnum32)GET_REG(cur_op, 4).n64;
                        MVMuint64 value = *(MVMuint32*)&num32;
                        if ((flags & 3) == MVM_SWITCHENDIAN) {
                            value = switch_endian(value, size);
                        }
                        REPR(buf)->pos_funcs.write_buf(tc, STABLE(buf), buf, OBJECT_BODY(buf),
                            (char*)&value
#if MVM_BIGENDIAN
                            + (8 - size)
#endif
                            , off, size);
                        break;
                    }
                    default: {
                        MVMuint64 value = *(MVMuint64*)&GET_REG(cur_op, 4).n64;
                        if ((flags & 3) == MVM_SWITCHENDIAN) {
                            value = switch_endian(value, size);
                        }
                        REPR(buf)->pos_funcs.write_buf(tc, STABLE(buf), buf, OBJECT_BODY(buf),
                            (char*)&value
#if MVM_BIGENDIAN
                            + (8 - size)
#endif
                            , off, size);
                        break;
                    }
                }
                MVM_SC_WB_OBJ(tc, buf);
                cur_op += 8;
                goto NEXT;
            }
            OP(serializetobuf): {
                MVMObject *sc   = GET_REG(cur_op, 2).o;
                MVMObject *obj  = GET_REG(cur_op, 4).o;
                MVMObject *type = GET_REG(cur_op, 6).o;
                if (REPR(sc)->ID != MVM_REPR_ID_SCRef)
                    MVM_exception_throw_adhoc(tc,
                        "Must provide an SCRef operand to serialize");
                GET_REG(cur_op, 0).o = MVM_serialization_serialize(
                    tc,
                    (MVMSerializationContext *)sc,
                    obj,
                    type
                );
                cur_op += 8;
                goto NEXT;
            }
            OP(readint):
            OP(readuint): {
                MVMObject*    const buf   = GET_REG(cur_op, 2).o;
                MVMint64      const off   = (MVMuint64)GET_REG(cur_op, 4).i64;
                MVMuint64     const flags = (MVMuint64)GET_REG(cur_op, 6).u64;
                unsigned char const size  = 1 << (flags >> 2);
                MVMuint64 val;
                if (!IS_CONCRETE(buf))
                    MVM_exception_throw_adhoc(tc, "Cannot read from a %s type object", MVM_6model_get_debug_name(tc, buf));
                if ((flags & 3) == 3 || size > 8) {
                    MVM_exception_throw_adhoc(tc, "Invalid flags value for %s",
                        (op == MVM_OP_readint ? "readint" : "readuint"));
                }

                val = REPR(buf)->pos_funcs.read_buf(tc, STABLE(buf), buf, OBJECT_BODY(buf), off, size);
                GET_REG(cur_op, 0).u64 = ((flags & 3) == MVM_SWITCHENDIAN) ? switch_endian(val, size) : val;

                if (op == MVM_OP_readint) {
                    if (size == 1) {
                        GET_REG(cur_op, 0).i64 = (MVMint64)(MVMint8)GET_REG(cur_op, 0).u64;
                    }
                    else if (size == 2) {
                        GET_REG(cur_op, 0).i64 = (MVMint64)(MVMint16)GET_REG(cur_op, 0).u64;
                    }
                    else if (size == 4) {
                        GET_REG(cur_op, 0).i64 = (MVMint64)(MVMint32)GET_REG(cur_op, 0).u64;
                    }
                }
                cur_op += 8;
                goto NEXT;
            }
            OP(readnum): {
                MVMObject*    const buf   = GET_REG(cur_op, 2).o;
                MVMint64      const off   = (MVMuint64)GET_REG(cur_op, 4).i64;
                MVMuint64     const flags = (MVMuint64)GET_REG(cur_op, 6).u64;
                unsigned char const size  = 1 << (flags >> 2);
                if (!IS_CONCRETE(buf))
                    MVM_exception_throw_adhoc(tc, "Cannot read from a %s type object", MVM_6model_get_debug_name(tc, buf));
                switch (size) {
                    case 8: {
                        MVMuint64 read = REPR(buf)->pos_funcs.read_buf(tc, STABLE(buf),
                                buf, OBJECT_BODY(buf), off, 8);
                        if ((flags & 3) == MVM_SWITCHENDIAN) {
                            read = switch_endian(read, size);
                        }
                        GET_REG(cur_op, 0).n64 = *(MVMnum64 *)&read;
                        break;
                    }
                    case 4: {
                        MVMuint32 read = (MVMuint32)REPR(buf)->pos_funcs.read_buf(tc, STABLE(buf),
                                buf, OBJECT_BODY(buf), off, 4);
                        if ((flags & 3) == MVM_SWITCHENDIAN) {
                            read = switch_endian(read, size);
                        }
                        GET_REG(cur_op, 0).n64 = *(MVMnum32 *)&read;
                        break;
                    }
                    default:
                        MVM_exception_throw_adhoc(tc, "Cannot read a num of %d bytes", size);
                }
                cur_op += 8;
                goto NEXT;
            }
            OP(uname):
                GET_REG(cur_op, 0).o = MVM_platform_uname(tc);
                cur_op += 2;
                goto NEXT;
            OP(freemem):
                GET_REG(cur_op, 0).i64 = MVM_platform_free_memory();
                cur_op += 2;
                goto NEXT;
            OP(totalmem):
                GET_REG(cur_op, 0).i64 = MVM_platform_total_memory();
                cur_op += 2;
                goto NEXT;
            OP(sp_guard): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                MVMSTable *want  = (MVMSTable *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 4)];
                cur_op += 10;
                if (!check || STABLE(check) != want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardconc): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                MVMSTable *want  = (MVMSTable *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 4)];
                cur_op += 10;
                if (!check || !IS_CONCRETE(check) || STABLE(check) != want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardtype): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                MVMSTable *want  = (MVMSTable *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 4)];
                cur_op += 10;
                if (!check || IS_CONCRETE(check) || STABLE(check) != want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardsf): {
                MVMObject *check = GET_REG(cur_op, 0).o;
                MVMStaticFrame *want = (MVMStaticFrame *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 2)];
                cur_op += 8;
                if (REPR(check)->ID != MVM_REPR_ID_MVMCode ||
                        ((MVMCode *)check)->body.sf != want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                goto NEXT;
            }
            OP(sp_guardsfouter): {
                MVMObject *check = GET_REG(cur_op, 0).o;
                MVMStaticFrame *want = (MVMStaticFrame *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 2)];
                cur_op += 8;
                if (REPR(check)->ID != MVM_REPR_ID_MVMCode ||
                        ((MVMCode *)check)->body.sf != want ||
                        ((MVMCode *)check)->body.outer != tc->cur_frame)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                goto NEXT;
            }
            OP(sp_guardobj): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                MVMObject *want = (MVMObject *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 4)];
                cur_op += 10;
                if (check != want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardnotobj): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                MVMObject *do_not_want = (MVMObject *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 4)];
                cur_op += 10;
                if (check == do_not_want)
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardjustconc): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                cur_op += 8;
                if (!IS_CONCRETE(check))
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_guardjusttype): {
                MVMRegister *target = &GET_REG(cur_op, 0);
                MVMObject *check = GET_REG(cur_op, 2).o;
                cur_op += 8;
                if (IS_CONCRETE(check))
                    MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                else
                    target->o = check;
                goto NEXT;
            }
            OP(sp_rebless):
                if (!REPR(GET_REG(cur_op, 2).o)->change_type) {
                    MVM_exception_throw_adhoc(tc, "This REPR cannot change type");
                }
                REPR(GET_REG(cur_op, 2).o)->change_type(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).o);
                GET_REG(cur_op, 0).o = GET_REG(cur_op, 2).o;
                MVM_SC_WB_OBJ(tc, GET_REG(cur_op, 0).o);
                cur_op += 10;
                MVM_spesh_deopt_all(tc);
                MVM_spesh_deopt_one(tc, GET_UI32(cur_op, -4));
                goto NEXT;
            OP(sp_resolvecode):
                GET_REG(cur_op, 0).o = MVM_frame_resolve_invokee_spesh(tc,
                    GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            OP(sp_decont): {
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMRegister *r = &GET_REG(cur_op, 0);
                cur_op += 4;
                if (obj && IS_CONCRETE(obj) && STABLE(obj)->container_spec)
                    STABLE(obj)->container_spec->fetch(tc, obj, r);
                else
                    r->o = obj;
                goto NEXT;
            }
            OP(sp_getlex_o): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 idx = GET_UI16(cur_op, 2);
                MVMuint16 outers = GET_UI16(cur_op, 4);
                MVMRegister found;
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                found = GET_LEX(cur_op, 2, f);
                GET_REG(cur_op, 0).o = found.o == NULL
                    ? MVM_frame_vivify_lexical(tc, f, idx)
                    : found.o;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_getlex_ins): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 idx = GET_UI16(cur_op, 2);
                MVMuint16 outers = GET_UI16(cur_op, 4);
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                GET_REG(cur_op, 0) = GET_LEX(cur_op, 2, f);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_getlex_no): {
                MVMRegister *found = MVM_frame_find_lexical_by_name(tc,
                    MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)), MVM_reg_obj);
                GET_REG(cur_op, 0).o = found ? found->o : tc->instance->VMNull;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bindlex_in): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 outers = GET_UI16(cur_op, 2);
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "bindlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                GET_LEX(cur_op, 0, f) = GET_REG(cur_op, 4);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bindlex_os): {
                MVMFrame *f = tc->cur_frame;
                MVMuint16 outers = GET_UI16(cur_op, 2);
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "bindlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
#if MVM_GC_DEBUG
                MVM_ASSERT_NOT_FROMSPACE(tc, GET_REG(cur_op, 4).o);
#endif
                MVM_ASSIGN_REF(tc, &(f->header), GET_LEX(cur_op, 0, f).o,
                    GET_REG(cur_op, 4).o);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_getarg_o):
                GET_REG(cur_op, 0).o = tc->cur_frame->params.args[GET_UI16(cur_op, 2)].o;
                cur_op += 4;
                goto NEXT;
            OP(sp_getarg_i):
                GET_REG(cur_op, 0).i64 = tc->cur_frame->params.args[GET_UI16(cur_op, 2)].i64;
                cur_op += 4;
                goto NEXT;
            OP(sp_getarg_n):
                GET_REG(cur_op, 0).n64 = tc->cur_frame->params.args[GET_UI16(cur_op, 2)].n64;
                cur_op += 4;
                goto NEXT;
            OP(sp_getarg_s):
                GET_REG(cur_op, 0).s = tc->cur_frame->params.args[GET_UI16(cur_op, 2)].s;
                cur_op += 4;
                goto NEXT;
            OP(sp_fastinvoke_v): {
                MVMCode     *code       = (MVMCode *)GET_REG(cur_op, 0).o;
                MVMRegister *args       = tc->cur_frame->args;
                MVMint32     spesh_cand = GET_UI16(cur_op, 2);
                tc->cur_frame->return_value = NULL;
                tc->cur_frame->return_type  = MVM_RETURN_VOID;
                cur_op += 4;
                tc->cur_frame->return_address = cur_op;
                MVM_frame_invoke(tc, code->body.sf, cur_callsite, args,
                    code->body.outer, (MVMObject *)code, spesh_cand);
                goto NEXT;
            }
            OP(sp_fastinvoke_i): {
                MVMCode     *code       = (MVMCode *)GET_REG(cur_op, 2).o;
                MVMRegister *args       = tc->cur_frame->args;
                MVMint32     spesh_cand = GET_UI16(cur_op, 4);
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type  = MVM_RETURN_INT;
                cur_op += 6;
                tc->cur_frame->return_address = cur_op;
                MVM_frame_invoke(tc, code->body.sf, cur_callsite, args,
                    code->body.outer, (MVMObject *)code, spesh_cand);
                goto NEXT;
            }
            OP(sp_fastinvoke_n): {
                MVMCode     *code       = (MVMCode *)GET_REG(cur_op, 2).o;
                MVMRegister *args       = tc->cur_frame->args;
                MVMint32     spesh_cand = GET_UI16(cur_op, 4);
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type  = MVM_RETURN_NUM;
                cur_op += 6;
                tc->cur_frame->return_address = cur_op;
                MVM_frame_invoke(tc, code->body.sf, cur_callsite, args,
                    code->body.outer, (MVMObject *)code, spesh_cand);
                goto NEXT;
            }
            OP(sp_fastinvoke_s): {
                MVMCode     *code       = (MVMCode *)GET_REG(cur_op, 2).o;
                MVMRegister *args       = tc->cur_frame->args;
                MVMint32     spesh_cand = GET_UI16(cur_op, 4);
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type  = MVM_RETURN_STR;
                cur_op += 6;
                tc->cur_frame->return_address = cur_op;
                MVM_frame_invoke(tc, code->body.sf, cur_callsite, args,
                    code->body.outer, (MVMObject *)code, spesh_cand);
                goto NEXT;
            }
            OP(sp_fastinvoke_o): {
                MVMCode     *code       = (MVMCode *)GET_REG(cur_op, 2).o;
                MVMRegister *args       = tc->cur_frame->args;
                MVMint32     spesh_cand = GET_UI16(cur_op, 4);
                tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                tc->cur_frame->return_type  = MVM_RETURN_OBJ;
                cur_op += 6;
                tc->cur_frame->return_address = cur_op;
                MVM_frame_invoke(tc, code->body.sf, cur_callsite, args,
                    code->body.outer, (MVMObject *)code, spesh_cand);
                goto NEXT;
            }
            OP(sp_speshresolve):
                MVM_spesh_plugin_resolve_spesh(tc, MVM_cu_string(tc, cu, GET_UI32(cur_op, 2)),
                    &GET_REG(cur_op, 0), GET_UI32(cur_op, 6),
                    (MVMStaticFrame *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 10)],
                    cur_op + 12, cur_callsite);
                goto NEXT;
            OP(sp_paramnamesused):
                MVM_args_throw_named_unused_error(tc, (MVMString *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 0)]);
                cur_op += 2;
                goto NEXT;
            OP(sp_getspeshslot):
                GET_REG(cur_op, 0).o = (MVMObject *)tc->cur_frame
                    ->effective_spesh_slots[GET_UI16(cur_op, 2)];
                cur_op += 4;
                goto NEXT;
            OP(sp_findmeth): {
                /* Obtain object and cache index; see if we get a match. */
                MVMObject *obj = GET_REG(cur_op, 2).o;
                MVMuint16  idx = GET_UI16(cur_op, 8);
                if ((MVMSTable *)tc->cur_frame->effective_spesh_slots[idx] == STABLE(obj)) {
                    GET_REG(cur_op, 0).o = (MVMObject *)tc->cur_frame->effective_spesh_slots[idx + 1];
                    cur_op += 10;
                }
                else {
                    /* May invoke, so pre-increment op counter */
                    MVMString *name = MVM_cu_string(tc, cu, GET_UI32(cur_op, 4));
                    MVMRegister *res = &GET_REG(cur_op, 0);
                    cur_op += 10;
                    MVM_6model_find_method_spesh(tc, obj, name, idx, res);

                }
                goto NEXT;
            }
            OP(sp_fastcreate):
                GET_REG(cur_op, 0).o = fastcreate(tc, cur_op);
                cur_op += 6;
                goto NEXT;
            OP(sp_get_o): {
                MVMObject *val = *((MVMObject **)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                GET_REG(cur_op, 0).o = val ? val : tc->instance->VMNull;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_get_i64):
                GET_REG(cur_op, 0).i64 = *((MVMint64 *)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_get_i32):
                GET_REG(cur_op, 0).i64 = *((MVMint32 *)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_get_i16):
                GET_REG(cur_op, 0).i64 = *((MVMint16 *)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_get_i8):
                GET_REG(cur_op, 0).i64 = *((MVMint8 *)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_get_n):
                GET_REG(cur_op, 0).n64 = *((MVMnum64 *)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_get_s):
                GET_REG(cur_op, 0).s = *((MVMString **)((char *)GET_REG(cur_op, 2).o + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            OP(sp_bind_o): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                MVMObject *value = GET_REG(cur_op, 4).o;
                MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)((char *)o + GET_UI16(cur_op, 2))), value);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_i64): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                *((MVMint64 *)((char *)o + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_i32): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                *((MVMint32 *)((char *)o + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_i16): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                *((MVMint16 *)((char *)o + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_i8): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                *((MVMint8 *)((char *)o + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_n): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                *((MVMnum64 *)((char *)o + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_s): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                MVMString *value = GET_REG(cur_op, 4).s;
                MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)((char *)o + GET_UI16(cur_op, 2))), value);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_bind_s_nowb): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                MVMString *value = GET_REG(cur_op, 4).s;
                *((MVMString **)((char *)o + GET_UI16(cur_op, 2))) = value;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6oget_o): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                MVMObject *val = MVM_p6opaque_read_object(tc, o, GET_UI16(cur_op, 4));
                GET_REG(cur_op, 0).o = val ? val : tc->instance->VMNull;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6ogetvt_o): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                MVMObject *val   = *((MVMObject **)(data + GET_UI16(cur_op, 4)));
                if (!val) {
                    val = (MVMObject *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 6)];
                    MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)(data + GET_UI16(cur_op, 4))), val);
                }
                GET_REG(cur_op, 0).o = val;
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_p6ogetvc_o): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                MVMObject *val   = *((MVMObject **)(data + GET_UI16(cur_op, 4)));
                if (!val) {
                    /* Clone might allocate, so re-fetch things after it. */
                    val  = MVM_repr_clone(tc, (MVMObject *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 6)]);
                    o    = GET_REG(cur_op, 2).o;
                    data = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                    MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)(data + GET_UI16(cur_op, 4))), val);
                }
                GET_REG(cur_op, 0).o = val;
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_p6oget_i): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                GET_REG(cur_op, 0).i64 = *((MVMint64 *)(data + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6oget_n): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                GET_REG(cur_op, 0).n64 = *((MVMnum64 *)(data + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6oget_s): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                GET_REG(cur_op, 0).s = *((MVMString **)(data + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6oget_bi): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                MVMP6bigintBody *bi = (MVMP6bigintBody *)(data + GET_UI16(cur_op, 4));
                GET_REG(cur_op, 0).i64 = MVM_BIGINT_IS_BIG(bi)
                    ? MVM_p6bigint_get_int64(tc, bi)
                    : bi->u.smallint.value;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6obind_o): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                MVMObject *value = GET_REG(cur_op, 4).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)(data + GET_UI16(cur_op, 2))), value);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6obind_i): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                *((MVMint64 *)(data + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6obind_n): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                *((MVMnum64 *)(data + GET_UI16(cur_op, 2))) = GET_REG(cur_op, 4).n64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6obind_s): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                MVM_ASSIGN_REF(tc, &(o->header), *((MVMString **)(data + GET_UI16(cur_op, 2))),
                    GET_REG(cur_op, 4).s);
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6oget_i32): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                GET_REG(cur_op, 0).i64 = *((MVMint32 *)(data + GET_UI16(cur_op, 4)));
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_p6obind_i32): {
                MVMObject *o     = GET_REG(cur_op, 0).o;
                char      *data  = MVM_p6opaque_real_data(tc, OBJECT_BODY(o));
                *((MVMint32 *)(data + GET_UI16(cur_op, 2))) = (MVMint32)GET_REG(cur_op, 4).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_getvt_o): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = (char *)o;
                MVMObject *val   = *((MVMObject **)(data + GET_UI16(cur_op, 4)));
                if (!val) {
                    val = (MVMObject *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 6)];
                    MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)(data + GET_UI16(cur_op, 4))), val);
                }
                GET_REG(cur_op, 0).o = val;
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_getvc_o): {
                MVMObject *o     = GET_REG(cur_op, 2).o;
                char      *data  = (char *)o;
                MVMObject *val   = *((MVMObject **)(data + GET_UI16(cur_op, 4)));
                if (!val) {
                    /* Clone might allocate, so re-fetch things after it. */
                    val  = MVM_repr_clone(tc,
                            (MVMObject *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 6)]);
                    o    = GET_REG(cur_op, 2).o;
                    data = (char *)o;
                    MVM_ASSIGN_REF(tc, &(o->header), *((MVMObject **)(data + GET_UI16(cur_op, 4))), val);
                }
                GET_REG(cur_op, 0).o = val;
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_fastbox_i): {
                MVMObject *obj = fastcreate(tc, cur_op);
                *((MVMint64 *)((char *)obj + GET_UI16(cur_op, 6))) = GET_REG(cur_op, 8).i64;
                GET_REG(cur_op, 0).o = obj;
                cur_op += 10;
                goto NEXT;
            }
            OP(sp_fastbox_bi): {
                MVMObject *obj = fastcreate(tc, cur_op);
                MVMP6bigintBody *body = (MVMP6bigintBody *)((char *)obj + GET_UI16(cur_op, 6));
                MVMint64 value = GET_REG(cur_op, 8).i64;
                if (MVM_IS_32BIT_INT(value)) {
                    body->u.smallint.value = (MVMint32)value;
                    body->u.smallint.flag = MVM_BIGINT_32_FLAG;
                }
                else {
                    MVM_p6bigint_store_as_mp_int(body, value);
                }
                GET_REG(cur_op, 0).o = obj;
                cur_op += 10;
                goto NEXT;
            }
            OP(sp_fastbox_i_ic): {
                MVMint64 value = GET_REG(cur_op, 8).i64;
                if (value >= -1 && value < 15) {
                    MVMint16 slot = GET_UI16(cur_op, 10);
                    GET_REG(cur_op, 0).o = tc->instance->int_const_cache->cache[slot][value + 1];
                }
                else {
                    MVMObject *obj = fastcreate(tc, cur_op);
                    *((MVMint64 *)((char *)obj + GET_UI16(cur_op, 6))) = value;
                    GET_REG(cur_op, 0).o = obj;
                }
                cur_op += 12;
                goto NEXT;
            }
            OP(sp_fastbox_bi_ic): {
                MVMint64 value = GET_REG(cur_op, 8).i64;
                if (value >= -1 && value < 15) {
                    MVMint16 slot = GET_UI16(cur_op, 10);
                    GET_REG(cur_op, 0).o = tc->instance->int_const_cache->cache[slot][value + 1];
                }
                else {
                    MVMObject *obj = fastcreate(tc, cur_op);
                    MVMP6bigintBody *body = (MVMP6bigintBody *)((char *)obj + GET_UI16(cur_op, 6));
                    if (MVM_IS_32BIT_INT(value)) {
                        body->u.smallint.value = (MVMint32)value;
                        body->u.smallint.flag = MVM_BIGINT_32_FLAG;
                    }
                    else {
                        MVM_p6bigint_store_as_mp_int(body, value);
                    }
                    GET_REG(cur_op, 0).o = obj;
                }
                cur_op += 12;
                goto NEXT;
            }
            OP(sp_deref_get_i64): {
                MVMObject *o      = GET_REG(cur_op, 2).o;
                MVMint64 **target = ((MVMint64 **)((char *)o + GET_UI16(cur_op, 4)));
                GET_REG(cur_op, 0).i64 = **target;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_deref_get_n): {
                MVMObject *o      = GET_REG(cur_op, 2).o;
                MVMnum64 **target = ((MVMnum64 **)((char *)o + GET_UI16(cur_op, 4)));
                GET_REG(cur_op, 0).n64 = **target;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_deref_bind_i64): {
                MVMObject *o      = GET_REG(cur_op, 0).o;
                MVMint64 **target = ((MVMint64 **)((char *)o + GET_UI16(cur_op, 4)));
                **target          = GET_REG(cur_op, 2).i64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_deref_bind_n): {
                MVMObject *o      = GET_REG(cur_op, 0).o;
                MVMnum64 **target = ((MVMnum64 **)((char *)o + GET_UI16(cur_op, 4)));
                **target          = GET_REG(cur_op, 2).n64;
                cur_op += 6;
                goto NEXT;
            }
            OP(sp_getlexvia_o): {
                MVMFrame *f = ((MVMCode *)GET_REG(cur_op, 6).o)->body.outer;
                MVMuint16 idx = GET_UI16(cur_op, 2);
                MVMuint16 outers = GET_UI16(cur_op, 4) - 1; /* - 1 as already in outer */
                MVMRegister found;
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                found = GET_LEX(cur_op, 2, f);
                GET_REG(cur_op, 0).o = found.o == NULL
                    ? MVM_frame_vivify_lexical(tc, f, idx)
                    : found.o;
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_getlexvia_ins): {
                MVMFrame *f = ((MVMCode *)GET_REG(cur_op, 6).o)->body.outer;
                MVMuint16 outers = GET_UI16(cur_op, 4) - 1; /* - 1 as already in outer */
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                GET_REG(cur_op, 0) = GET_LEX(cur_op, 2, f);
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_bindlexvia_os): {
                MVMFrame *f = ((MVMCode *)GET_REG(cur_op, 4).o)->body.outer;
                MVMuint16 outers = GET_UI16(cur_op, 2) - 1; /* - 1 as already in outer */
                MVMRegister found;
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
#if MVM_GC_DEBUG
                MVM_ASSERT_NOT_FROMSPACE(tc, GET_REG(cur_op, 6).o);
#endif
                MVM_ASSIGN_REF(tc, &(f->header), GET_LEX(cur_op, 0, f).o,
                    GET_REG(cur_op, 6).o);
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_bindlexvia_in): {
                MVMFrame *f = ((MVMCode *)GET_REG(cur_op, 4).o)->body.outer;
                MVMuint16 outers = GET_UI16(cur_op, 2) - 1; /* - 1 as already in outer */
                MVMRegister found;
                while (outers) {
                    if (!f->outer)
                        MVM_exception_throw_adhoc(tc, "getlex: outer index out of range");
                    f = f->outer;
                    outers--;
                }
                GET_LEX(cur_op, 0, f) = GET_REG(cur_op, 6);
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_getstringfrom): {
                MVMCompUnit *dep = (MVMCompUnit *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 2)];
                MVMuint32 idx = GET_UI32(cur_op, 4);
                GET_REG(cur_op, 0).s = MVM_cu_string(tc, dep, idx);
                cur_op += 8;
                goto NEXT;
            }
            OP(sp_getwvalfrom): {
                MVMSerializationContext *dep = (MVMSerializationContext *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 2)];
                MVMuint64 idx = MVM_BC_get_I64(cur_op, 4);
                GET_REG(cur_op, 0).o = MVM_sc_get_object(tc, dep, idx);
                cur_op += 12;
                goto NEXT;
            }
            OP(sp_jit_enter): {
                MVMJitCode *jc = tc->cur_frame->spesh_cand->jitcode;
                if (MVM_UNLIKELY(jc == NULL)) {
                    MVM_exception_throw_adhoc(tc, "Try to enter NULL jitcode");
                }
                /* trampoline back to this opcode */
                cur_op -= 2;
                MVM_jit_code_enter(tc, jc, cu);
                if (MVM_UNLIKELY(!tc->cur_frame)) {
                    /* somehow unwound our top frame */
                    goto return_label;
                }
                goto NEXT;
            }
            OP(sp_boolify_iter): {
                GET_REG(cur_op, 0).i64 = MVM_iter_istrue(tc, (MVMIter*)GET_REG(cur_op, 2).o);
                cur_op += 4;
                goto NEXT;
            }
            OP(sp_boolify_iter_arr): {
                MVMIter *iter = (MVMIter *)GET_REG(cur_op, 2).o;

                GET_REG(cur_op, 0).i64 = iter->body.array_state.index + 1 < iter->body.array_state.limit ? 1 : 0;

                cur_op += 4;
                goto NEXT;
            }
            OP(sp_boolify_iter_hash): {
                MVMIter *iter = (MVMIter *)GET_REG(cur_op, 2).o;

                GET_REG(cur_op, 0).i64 = iter->body.hash_state.next != NULL ? 1 : 0;

                cur_op += 4;
                goto NEXT;
            }
            OP(sp_cas_o): {
                MVMRegister *result = &GET_REG(cur_op, 0);
                MVMObject *target = GET_REG(cur_op, 2).o;
                MVMObject *expected = GET_REG(cur_op, 4).o;
                MVMObject *value = GET_REG(cur_op, 6).o;
                cur_op += 8;
                target->st->container_spec->cas(tc, target, expected, value, result);
                goto NEXT;
            }
            OP(sp_atomicload_o): {
                MVMObject *target = GET_REG(cur_op, 2).o;
                GET_REG(cur_op, 0).o = target->st->container_spec->atomic_load(tc, target);
                cur_op += 4;
                goto NEXT;
            }
            OP(sp_atomicstore_o): {
                MVMObject *target = GET_REG(cur_op, 0).o;
                MVMObject *value = GET_REG(cur_op, 2).o;
                cur_op += 4;
                target->st->container_spec->atomic_store(tc, target, value);
                goto NEXT;
            }
            OP(sp_add_I): {
                MVMuint16 offset = GET_UI16(cur_op, 10);
                MVMP6bigintBody *ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                MVMP6bigintBody *bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                MVMP6bigintBody *bc;
                MVMObject *result_obj = NULL;
                if (ba->u.smallint.flag == MVM_BIGINT_32_FLAG && bb->u.smallint.flag == MVM_BIGINT_32_FLAG) {
                    MVMuint64 result = (MVMint64)ba->u.smallint.value + (MVMint64)bb->u.smallint.value;
                    if (MVM_IS_32BIT_INT(result)) {
                        if (result < -1 || result >= 15) {
                            result_obj = fastcreate(tc, cur_op);
                            bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                            bc->u.smallint.value = (MVMint32)result;
                            bc->u.smallint.flag = MVM_BIGINT_32_FLAG;
                        }
                        else {
                            result_obj = tc->instance->int_const_cache->cache[GET_UI16(cur_op, 12)][result + 1];
                        }
                    }
                }
                if (!result_obj) {
                    result_obj = fastcreate(tc, cur_op);
                    ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                    bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                    bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                    MVM_bigint_fallback_add(tc, ba, bb, bc);
                }
                GET_REG(cur_op, 0).o = result_obj;
                cur_op += 14;
                goto NEXT;
            }
            OP(sp_sub_I): {
                MVMuint16 offset = GET_UI16(cur_op, 10);
                MVMP6bigintBody *ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                MVMP6bigintBody *bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                MVMP6bigintBody *bc;
                MVMObject *result_obj = NULL;
                if (ba->u.smallint.flag == MVM_BIGINT_32_FLAG && bb->u.smallint.flag == MVM_BIGINT_32_FLAG) {
                    MVMuint64 result = (MVMint64)ba->u.smallint.value - (MVMint64)bb->u.smallint.value;
                    if (MVM_IS_32BIT_INT(result)) {
                        if (result < -1 || result >= 15) {
                            result_obj = fastcreate(tc, cur_op);
                            bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                            bc->u.smallint.value = (MVMint32)result;
                            bc->u.smallint.flag = MVM_BIGINT_32_FLAG;
                        }
                        else {
                            result_obj = tc->instance->int_const_cache->cache[GET_UI16(cur_op, 12)][result + 1];
                        }
                    }
                }
                if (!result_obj) {
                    result_obj = fastcreate(tc, cur_op);
                    ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                    bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                    bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                    MVM_bigint_fallback_sub(tc, ba, bb, bc);
                }
                GET_REG(cur_op, 0).o = result_obj;
                cur_op += 14;
                goto NEXT;
            }
            OP(sp_mul_I): {
                MVMuint16 offset = GET_UI16(cur_op, 10);
                MVMP6bigintBody *ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                MVMP6bigintBody *bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                MVMP6bigintBody *bc;
                MVMObject *result_obj = NULL;
                if (ba->u.smallint.flag == MVM_BIGINT_32_FLAG && bb->u.smallint.flag == MVM_BIGINT_32_FLAG) {
                    MVMuint64 result = (MVMint64)ba->u.smallint.value * (MVMint64)bb->u.smallint.value;
                    if (MVM_IS_32BIT_INT(result)) {
                        if (result < -1 || result >= 15) {
                            result_obj = fastcreate(tc, cur_op);
                            bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                            bc->u.smallint.value = (MVMint32)result;
                            bc->u.smallint.flag = MVM_BIGINT_32_FLAG;
                        }
                        else {
                            result_obj = tc->instance->int_const_cache->cache[GET_UI16(cur_op, 12)][result + 1];
                        }
                    }
                }
                if (!result_obj) {
                    result_obj = fastcreate(tc, cur_op);
                    ba = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 6).o + offset);
                    bb = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 8).o + offset);
                    bc = (MVMP6bigintBody *)((char *)result_obj + offset);
                    MVM_bigint_fallback_mul(tc, ba, bb, bc);
                }
                GET_REG(cur_op, 0).o = result_obj;
                cur_op += 14;
                goto NEXT;
            }
            OP(sp_bool_I): {
                MVMuint16 offset = GET_UI16(cur_op, 4);
                MVMP6bigintBody *b = (MVMP6bigintBody *)((char *)GET_REG(cur_op, 2).o + offset);
                MVMuint64 result = 0;
                if (b->u.smallint.flag == MVM_BIGINT_32_FLAG) {
                    result = (MVMint64)b->u.smallint.value != 0;
                }
                else if (b->u.smallint.flag != MVM_BIGINT_32_FLAG) {
                    result = !mp_iszero(b->u.bigint);
                }
                GET_REG(cur_op, 0).i64 = result;
                cur_op += 6;
                goto NEXT;
            }
            OP(prof_enter):
                MVM_profile_log_enter(tc, tc->cur_frame->static_info,
                    MVM_PROFILE_ENTER_NORMAL);
                goto NEXT;
            OP(prof_enterspesh):
                MVM_profile_log_enter(tc, tc->cur_frame->static_info,
                    MVM_PROFILE_ENTER_SPESH);
                goto NEXT;
            OP(prof_enterinline):
                MVM_profile_log_enter(tc,
                    (MVMStaticFrame *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 0)],
                    MVM_PROFILE_ENTER_SPESH_INLINE);
                cur_op += 2;
                goto NEXT;
            OP(prof_enternative):
                MVM_profile_log_enter_native(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(prof_exit):
                MVM_profile_log_exit(tc);
                goto NEXT;
            OP(prof_allocated):
                MVM_profile_log_allocated(tc, GET_REG(cur_op, 0).o);
                cur_op += 2;
                goto NEXT;
            OP(prof_replaced):
                MVM_profile_log_scalar_replaced(tc,
                    (MVMSTable *)tc->cur_frame->effective_spesh_slots[GET_UI16(cur_op, 0)]);
                cur_op += 2;
                goto NEXT;
            OP(ctw_check): {
                MVMObject *obj = GET_REG(cur_op, 0).o;
                MVMint16 blame = GET_I16(cur_op, 2);
                cur_op += 4;
                MVM_cross_thread_write_check(tc, obj, blame);
                goto NEXT;
            }
            /* The compiler compiles faster if all deprecated are together and at the end
             * even though the op numbers are technically out of order. */
            OP(DEPRECATED_4):
            OP(DEPRECATED_5):
            OP(DEPRECATED_6):
            OP(DEPRECATED_7):
            OP(DEPRECATED_8):
            OP(DEPRECATED_9):
            OP(DEPRECATED_10):
            OP(DEPRECATED_11):
            OP(DEPRECATED_12):
                MVM_exception_throw_adhoc(tc, "The getregref_* ops were removed in MoarVM 2017.01.");
            OP(DEPRECATED_14):
                MVM_exception_throw_adhoc(tc, "The asyncwritestr op was removed in MoarVM 2017.05.");
            OP(DEPRECATED_15):
                MVM_exception_throw_adhoc(tc, "The asyncwritestrto op was removed in MoarVM 2017.05.");
            OP(DEPRECATED_16):
                MVM_exception_throw_adhoc(tc, "The asyncreadchars op was removed in MoarVM 2017.05.");
            OP(DEPRECATED_17):
                MVM_exception_throw_adhoc(tc, "The setencoding op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_18):
                MVM_exception_throw_adhoc(tc, "The write_fhs op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_19):
                MVM_exception_throw_adhoc(tc, "The say_fhs op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_21):
                MVM_exception_throw_adhoc(tc, "The readlinechomp_fh op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_22):
                MVM_exception_throw_adhoc(tc, "The readall_fh op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_23):
                MVM_exception_throw_adhoc(tc, "The read_fhs op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_24):
                MVM_exception_throw_adhoc(tc, "The setinputlinesep op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_25):
                MVM_exception_throw_adhoc(tc, "The setinputlineseps op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_27):
                MVM_exception_throw_adhoc(tc, "The slurp op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_28):
                MVM_exception_throw_adhoc(tc, "The spew op was removed in MoarVM 2017.06.");
            OP(DEPRECATED_29):
                MVM_exception_throw_adhoc(tc, "The spawn op was removed in MoarVM 2017.07.");
            OP(DEPRECATED_30):
                MVM_exception_throw_adhoc(tc, "The shell op was removed in MoarVM 2017.07.");
            OP(DEPRECATED_31):
                MVM_exception_throw_adhoc(tc, "The syncpipe op was removed in MoarVM 2017.07.");
            OP(DEPRECATED_32):
                MVM_exception_throw_adhoc(tc, "The close_fhi op was removed in MoarVM 2017.07.");
            OP(DEPRECATED_33):
                MVM_exception_throw_adhoc(tc, "The newlexotic op was removed in MoarVM 2017.08.");
            OP(DEPRECATED_34):
                MVM_exception_throw_adhoc(tc, "The lexoticresult op was removed in MoarVM 2017.08.");
            OP(coverage_log): {
                MVMString *filename = MVM_cu_string(tc, cu, GET_UI32(cur_op, 0));
                MVMuint32 lineno    = GET_UI32(cur_op, 4);
                MVMuint32 cacheidx  = GET_UI32(cur_op, 8);
                char      *cache    = (char *)(uintptr_t)MVM_BC_get_I64(cur_op, 12);
                MVM_line_coverage_report(tc, filename, lineno, cacheidx, cache);
                cur_op += 20;
                goto NEXT;
            }
            OP(breakpoint): {
                MVMuint32 file_idx = GET_UI32(cur_op, 0);
                MVMuint32 line_no  = GET_UI32(cur_op, 4);
                MVM_debugserver_breakpoint_check(tc, file_idx, line_no);
                cur_op += 8;
                goto NEXT;
            }
#if MVM_CGOTO
            OP_CALL_EXTOP: {
                /* Bounds checking? Never heard of that. */
                MVMuint8 *op_before = cur_op;
                MVMExtOpRecord *record = &cu->body.extops[op - MVM_OP_EXT_BASE];
                record->func(tc, cur_op);
                if (op_before == cur_op)
                    cur_op += record->operand_bytes;
                goto NEXT;
            }
#else
            default: {
                if (op >= MVM_OP_EXT_BASE
                        && (op - MVM_OP_EXT_BASE) < cu->body.num_extops) {
                    MVMuint8 *op_before = cur_op;
                    MVMExtOpRecord *record =
                            &cu->body.extops[op - MVM_OP_EXT_BASE];
                    record->func(tc, cur_op);
                    if (op_before == cur_op)
                        cur_op += record->operand_bytes;
                    goto NEXT;
                }

                MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) opcode %u", op);
            }
#endif
        }
    }

    return_label:
    /* Need to clear these pointer pointers since they may be rooted
     * by some GC procedure. */
    tc->interp_cur_op         = NULL;
    tc->interp_bytecode_start = NULL;
    tc->interp_reg_base       = NULL;
    tc->interp_cu             = NULL;
    MVM_barrier();
}

void MVM_interp_enable_tracing() {
    tracing_enabled = 1;
}
