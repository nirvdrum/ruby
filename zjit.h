#ifndef ZJIT_H
#define ZJIT_H 1
//
// This file contains definitions ZJIT exposes to the CRuby codebase
//

#include "shape.h" // for shape_id_t

// ZJIT_STATS controls whether to support runtime counters in the interpreter
#ifndef ZJIT_STATS
# define ZJIT_STATS (USE_ZJIT && RUBY_DEBUG)
#endif

// Stack map entries are either immediate Ruby VALUEs, tagged native-stack
// locations, or tagged skip counts. Stack maps never contain heap VALUEs, so
// these tags are available: they are not Qfalse (0), and their low 3 bits are
// zero, so RB_SPECIAL_CONST_P is false.
#define ZJIT_STACK_MAP_VREG_TAG 0x08
#define ZJIT_STACK_MAP_SKIP_TAG 0x10
#define ZJIT_STACK_MAP_TAG_MASK 0xff
#define ZJIT_STACK_MAP_SHIFT 8

static inline bool
ZJIT_STACK_MAP_VREG_P(VALUE entry)
{
    return (entry & ZJIT_STACK_MAP_TAG_MASK) == ZJIT_STACK_MAP_VREG_TAG;
}

static inline size_t
ZJIT_STACK_MAP_VREG_INDEX(VALUE entry)
{
    return entry >> ZJIT_STACK_MAP_SHIFT;
}

static inline bool
ZJIT_STACK_MAP_SKIP_P(VALUE entry)
{
    return (entry & ZJIT_STACK_MAP_TAG_MASK) == ZJIT_STACK_MAP_SKIP_TAG;
}

static inline size_t
ZJIT_STACK_MAP_SKIP_SIZE(VALUE entry)
{
    return entry >> ZJIT_STACK_MAP_SHIFT;
}

// Describes one logical frame in an inline chain. All but the final entry of
// a chain are inlined callees that have no physical rb_control_frame_t while
// JIT code runs; the final entry describes the physical frame itself and
// uses only its iseq/pc members. The whole chain is known statically at each
// JITFrame site, so these are baked at compile time and live for the process
// lifetime, like JITFrames themselves. Design notes for the virtual inline
// frames work will be folded into doc/jit/zjit.md once it is complete.
//
// Terminology: this is the analogue of HotSpot's compiledVFrame/ScopeDesc, a
// descriptor from which a frame can be synthesized. It is not the analogue
// of Truffle's VirtualFrame, which is a live frame object that the compiler
// scalar-replaces.
typedef struct zjit_inline_frame {
    // PC within iseq. For the innermost frame this duplicates the owning
    // JITFrame's pc; for outer frames it is the call site's return PC, which
    // is fully static per JITFrame site. Field order matches zjit_jit_frame
    // for the members the two structs share.
    const VALUE *pc;
    // The inlined callee's ISEQ.
    const rb_iseq_t *iseq;
    // Method entry to materialize into ep[-2].
    const rb_callable_method_entry_t *cme;
    // VM_FRAME_MAGIC_* | flags to materialize into ep[0].
    VALUE frame_type;
    // Static part of ep[-1]: VM_BLOCK_HANDLER_NONE or a tagged captured EP
    // for bmethods. Literal blockiseq handlers cannot be encoded statically
    // because they capture the caller frame; materialization reconstructs
    // them from the caller's freshly synthesized frame instead.
    VALUE specval;
    // Receiver location in the ZJIT_STACK_MAP_* encoding: an immediate VALUE
    // or a tagged native-stack index from cfp->jit_return.
    //
    // Frame layout details that materialization also needs, such as the
    // callee's local table size, are intentionally not duplicated here:
    // materialization runs in-thread with full VM access and derives them
    // from iseq, keeping the ISEQ the single source of truth. Only the
    // read-only walkers, which need nothing beyond iseq/pc, run in
    // restricted contexts.
    VALUE recv;
    // This frame's VM stack offset, in slots, from the physical frame's SP
    // register (which equals the physical frame's ep + 1). Zero for the
    // final (physical frame) entry. Unlike local table sizes this is not
    // derivable from the ISEQ: it depends on each call site's operand stack
    // depth, so materialization needs it to lay out the synthesized frames.
    uint32_t sp_base;
} zjit_inline_frame_t;

// JITFrame is defined here as the single source of truth and imported into
// Rust via bindgen. C code reads fields directly; Rust uses an impl block.
typedef struct zjit_jit_frame {
    // Program counter for this frame, used for backtraces and GC.
    // NULL for C frames (they don't have a Ruby PC).
    const VALUE *pc;
    // The ISEQ this frame belongs to. Marked via rb_execution_context_mark.
    // NULL for C frames.
    const rb_iseq_t *iseq;
    // Whether to materialize block_code when this frame is materialized.
    // True when the ISEQ doesn't contain send/invokesuper/invokeblock
    // (which write block_code themselves), so we must restore it.
    // Always false for C frames.
    bool materialize_block_code;

    // Number of stack map entries in stack[].
    uint32_t stack_size;

    // Number of entries in inline_frames. Zero when no inlining is active
    // at this site, in which case inline_frames may be NULL and the
    // JITFrame's pc/iseq describe the only logical frame. When non-zero, the
    // chain describes every logical frame this physical CFP represents,
    // innermost (deepest inlined callee) first; the final entry describes
    // the physical frame itself, whose pc is the static call site into the
    // first inlined callee. The innermost entry's pc/iseq duplicate the
    // JITFrame's pc/iseq fields.
    uint32_t inline_count;
    // Distance in VALUE slots from the physical frame's initial stack
    // pointer (the value ZJIT pins its SP register to) to the stack pointer
    // the publishing site saves into cfp->sp. Every site that publishes this
    // JITFrame writes cfp->sp in the same instruction sequence, so
    // materialization can recover the base as cfp->sp - sp_offset. The base
    // cannot be derived from cfp->ep because the environment can escape to
    // the heap (Binding or block capture) while the frame is still live, at
    // which point cfp->ep no longer points into the VM stack. Only
    // meaningful when inline_count is non-zero.
    uint32_t sp_offset;
    // Compile-time descriptor chain, or NULL. Owned by ZJIT and registered
    // as a GC root so iseq/cme members stay alive and are updated on
    // compaction.
    const zjit_inline_frame_t *inline_frames;

    // Flexible array of stack map entries. Each entry is either an immediate
    // VALUE, a tagged native-stack index from cfp->jit_return for a value
    // kept by the JIT, or a tagged count of VM stack slots to skip.
    VALUE stack[];
} zjit_jit_frame_t;

#if USE_ZJIT
extern void *rb_zjit_entry;
extern const zjit_jit_frame_t rb_zjit_c_frame;
extern uint64_t rb_zjit_call_threshold;
extern uint64_t rb_zjit_profile_threshold;
void rb_zjit_compile_iseq(const rb_iseq_t *iseq, rb_execution_context_t *ec, bool jit_exception);
void rb_zjit_profile_insn(uint32_t insn, rb_execution_context_t *ec);
void rb_zjit_profile_enable(const rb_iseq_t *iseq);
void rb_zjit_bop_redefined(int redefined_flag, enum ruby_basic_operators bop);
void rb_zjit_cme_invalidate(const rb_callable_method_entry_t *cme);
void rb_zjit_cme_free(const rb_callable_method_entry_t *cme);
void rb_zjit_klass_free(VALUE klass);
void rb_zjit_invalidate_no_ep_escape(const rb_iseq_t *iseq);
void rb_zjit_constant_state_changed(ID id);
void rb_zjit_iseq_mark(void *payload);
void rb_zjit_iseq_update_references(void *payload);
void rb_zjit_mark_all_writable(void);
void rb_zjit_mark_all_executable(void);
void rb_zjit_iseq_free(const rb_iseq_t *iseq);
void rb_zjit_invalidate_single_ractor(void);
void rb_zjit_tracing_invalidate_all(void);
void rb_zjit_invalidate_no_singleton_class(VALUE klass);
void rb_zjit_invalidate_root_box(void);
void rb_zjit_jit_frame_update_references(zjit_jit_frame_t *jit_frame);
void rb_zjit_materialize_frames(const rb_execution_context_t *ec, rb_control_frame_t *cfp);
void rb_zjit_materialize_frames_for_longjmp(const rb_execution_context_t *ec, rb_control_frame_t *cfp);
size_t rb_zjit_hash_new_size(VALUE *flags_out);
bool rb_zjit_class_allocate_instance_fastpath(VALUE klass, size_t *size_out, shape_id_t *shape_id_out);
bool rb_zjit_str_resurrect_fastpath(VALUE str, bool chilled, size_t *size_out, VALUE *flags_out, long *len_out, size_t *byte_size_out);
bool rb_zjit_array_dup_can_fastpath(VALUE ary, size_t *alloc_size_out, VALUE *flags_out, long *len_out);
void rb_zjit_range_new_fastpath(bool exclude_end, size_t *alloc_size_out, VALUE *flags_out);

// Special value for cfp->jit_return that means "this is a C method frame, use
// rb_zjit_c_frame as the JITFrame". We don't control the native stack layout
// for C frames, so there's no per-call JITFrame storage; we set this sentinel
// instead of a heap-allocated JITFrame pointer.
#define ZJIT_JIT_RETURN_C_FRAME 0x1

static inline const zjit_jit_frame_t *
CFP_ZJIT_FRAME(const rb_control_frame_t *cfp)
{
    if ((VALUE)cfp->jit_return == ZJIT_JIT_RETURN_C_FRAME) {
        return &rb_zjit_c_frame;
    }
    else {
        // Read JITFrame from this frame's stack slot. cfp->jit_return points at
        // the slot reserved for this frame's inlining depth, so distinct frames in
        // the same JIT function read distinct slots. An initial frame describing
        // the entry PC + iseq is written by gen_entry_point() for the top-level
        // frame and by gen_push_lightweight_frame() for inlined frames. That entry
        // PC is correct only at the frame's start; because the PC this frame reports
        // must track where execution currently is, later gen_save_pc_for_gc() calls
        // rewrite the slot with the live PC as execution advances through the frame,
        // before any non-leaf C call.
        return (const zjit_jit_frame_t *)((VALUE *)cfp->jit_return)[-1];
    }
}
#else
#define rb_zjit_entry 0
static inline void rb_zjit_compile_iseq(const rb_iseq_t *iseq, rb_execution_context_t *ec, bool jit_exception) {}
static inline void rb_zjit_profile_insn(uint32_t insn, rb_execution_context_t *ec) {}
static inline void rb_zjit_profile_enable(const rb_iseq_t *iseq) {}
static inline void rb_zjit_bop_redefined(int redefined_flag, enum ruby_basic_operators bop) {}
static inline void rb_zjit_cme_invalidate(const rb_callable_method_entry_t *cme) {}
static inline void rb_zjit_invalidate_no_ep_escape(const rb_iseq_t *iseq) {}
static inline void rb_zjit_constant_state_changed(ID id) {}
static inline void rb_zjit_invalidate_single_ractor(void) {}
static inline void rb_zjit_tracing_invalidate_all(void) {}
static inline void rb_zjit_invalidate_no_singleton_class(VALUE klass) {}
static inline void rb_zjit_invalidate_root_box(void) {}
static inline void rb_zjit_jit_frame_update_references(zjit_jit_frame_t *jit_frame) {}
static inline void rb_zjit_materialize_frames(const rb_execution_context_t *ec, rb_control_frame_t *cfp) {}
static inline void rb_zjit_materialize_frames_for_longjmp(const rb_execution_context_t *ec, rb_control_frame_t *cfp) {}
static inline const zjit_jit_frame_t *CFP_ZJIT_FRAME(const rb_control_frame_t *cfp) { return NULL; }
#endif // #if USE_ZJIT

#define rb_zjit_enabled_p (rb_zjit_entry != 0)

// Return true if a given CFP has ZJIT's JITFrame.
static inline bool
CFP_ZJIT_FRAME_P(const rb_control_frame_t *cfp)
{
    if (!rb_zjit_enabled_p) return false;
    return cfp->jit_return != NULL;
}

static inline const VALUE*
CFP_PC(const rb_control_frame_t *cfp)
{
    if (CFP_ZJIT_FRAME_P(cfp)) {
        return CFP_ZJIT_FRAME(cfp)->pc;
    }
    return cfp->pc;
}

static inline const rb_iseq_t*
CFP_ISEQ(const rb_control_frame_t *cfp)
{
    if (CFP_ZJIT_FRAME_P(cfp)) {
        return CFP_ZJIT_FRAME(cfp)->iseq;
    }
    return cfp->_iseq;
}

// Read-only iterator over the logical frames represented by one physical
// CFP: each virtual (inlined) frame innermost first, then the physical frame
// itself. Safe to use from restricted contexts such as signal handlers
// (rb_profile_frames) and crash dumps because it never writes and touches
// only the JITFrame and its compile-time descriptors. Frames that hold no
// JITFrame yield exactly one entry, so walkers can use this unconditionally.
typedef struct {
    const rb_control_frame_t *cfp;
    const zjit_jit_frame_t *jit_frame; // NULL for non-ZJIT frames
    uint32_t next;                     // next chain entry to yield
    bool done;
} zjit_frame_iter_t;

static inline zjit_frame_iter_t
zjit_frame_iter_init(const rb_control_frame_t *cfp)
{
    zjit_frame_iter_t iter = {
        .cfp = cfp,
        .jit_frame = CFP_ZJIT_FRAME_P(cfp) ? CFP_ZJIT_FRAME(cfp) : NULL,
        .next = 0,
        .done = false,
    };
    return iter;
}

// Yield the next logical frame's iseq, pc, and cme. Returns false when
// exhausted. Virtual frames supply the cme from their descriptor because
// they have no EP to look it up through; for the physical frame cme_out is
// set to NULL and the caller should use its usual EP-based lookup
// (rb_vm_frame_method_entry). For a frame without a JITFrame the reported
// iseq/pc fall back to cfp->_iseq and cfp->pc, matching CFP_ISEQ/CFP_PC.
static inline bool
zjit_frame_iter_next(zjit_frame_iter_t *iter, const rb_iseq_t **iseq_out, const VALUE **pc_out, const rb_callable_method_entry_t **cme_out)
{
    if (iter->done) {
        return false;
    }

#if USE_ZJIT
    const zjit_jit_frame_t *jit_frame = iter->jit_frame;
    if (jit_frame && iter->next < jit_frame->inline_count) {
        const zjit_inline_frame_t *vframe = &jit_frame->inline_frames[iter->next++];
        // The final chain entry describes the physical frame; report its cme
        // as NULL like the no-chain case so the caller's EP-based lookup,
        // which works for physical frames, stays authoritative.
        bool physical = iter->next == jit_frame->inline_count;
        *iseq_out = vframe->iseq;
        *pc_out = vframe->pc;
        *cme_out = physical ? NULL : vframe->cme;
        if (physical) {
            iter->done = true;
        }
        return true;
    }

    // A frame with no inlining: yield the physical frame itself.
    iter->done = true;
    if (jit_frame) {
        *iseq_out = jit_frame->iseq;
        *pc_out = jit_frame->pc;
    }
    else {
        *iseq_out = iter->cfp->_iseq;
        *pc_out = iter->cfp->pc;
    }
    *cme_out = NULL;
    return true;
#else
    iter->done = true;
    *iseq_out = iter->cfp->_iseq;
    *pc_out = iter->cfp->pc;
    *cme_out = NULL;
    return true;
#endif
}

#endif // #ifndef ZJIT_H
