use std::alloc::{alloc, handle_alloc_error, Layout};
use std::mem::{align_of, size_of};
use std::ptr;

use crate::cruby::{__IncompleteArrayField, IseqPtr, VALUE, rb_gc_mark_movable, rb_gc_location};
use crate::cruby::{zjit_jit_frame, zjit_inline_frame};
use crate::codegen::iseq_may_write_block_code;
use crate::state::ZJITState;

/// JITFrame struct is defined in zjit.h (the single source of truth) and
/// imported into Rust via bindgen. See zjit.h for field documentation.
pub type JITFrame = zjit_jit_frame;

/// One entry of a JITFrame's inline chain. Defined in zjit.h (the single
/// source of truth) and imported via bindgen; see zjit.h for field
/// documentation.
pub type InlineFrame = zjit_inline_frame;

impl JITFrame {
    /// Allocate a JITFrame and its trailing stack map on the heap, register it
    /// with ZJITState, and return a raw pointer that remains valid for the
    /// lifetime of the process.
    fn alloc(
        pc: *const VALUE,
        iseq: IseqPtr,
        materialize_block_code: bool,
        stack_size: usize,
    ) -> *const Self {
        // JITFrame ends with a flexible stack[] array, so allocate enough
        // space for the fixed fields plus the requested stack map entries.
        let frame_size = size_of::<JITFrame>()
            .checked_add(stack_size.checked_mul(size_of::<VALUE>()).unwrap())
            .unwrap();
        let layout = Layout::from_size_align(frame_size, align_of::<JITFrame>()).unwrap();
        let raw_ptr = unsafe { alloc(layout) as *mut JITFrame };
        if raw_ptr.is_null() {
            handle_alloc_error(layout);
        }

        unsafe {
            ptr::write(raw_ptr, JITFrame {
                pc,
                iseq,
                materialize_block_code,
                stack_size: stack_size.try_into().unwrap(),
                inline_count: 0,
                sp_offset: 0,
                inline_frames: ptr::null(),
                stack: __IncompleteArrayField::new(),
            });
        }
        ZJITState::get_jit_frames().push(raw_ptr);
        raw_ptr as *const _
    }

    /// Create a JITFrame for an ISEQ frame.
    pub fn new_iseq(pc: *const VALUE, iseq: IseqPtr, stack_size: usize) -> *const Self {
        let materialize_block_code = !iseq_may_write_block_code(iseq);
        Self::alloc(pc, iseq, materialize_block_code, stack_size)
    }

    /// Create a JITFrame that carries an inline chain describing every
    /// logical frame live at this site, innermost first, with the final
    /// entry describing the physical frame. The chain is leaked because the
    /// JITFrame referencing it lives for the rest of the process; both are
    /// kept alive for GC through ZJITState::get_jit_frames().
    ///
    /// The chain must be non-empty and its first entry's pc/iseq must match
    /// the pc/iseq passed here, which continue to describe the innermost
    /// frame for CFP_PC/CFP_ISEQ compatibility.
    /// `sp_offset` is the distance in VALUE slots from the physical frame's
    /// initial stack pointer to the cfp->sp value the publishing site saves,
    /// letting materialization recover the base even after the frame's
    /// environment escapes to the heap (see zjit_jit_frame_t in zjit.h).
    pub fn new_iseq_with_chain(
        pc: *const VALUE,
        iseq: IseqPtr,
        stack_size: usize,
        chain: Vec<InlineFrame>,
        sp_offset: usize,
    ) -> *const Self {
        assert!(!chain.is_empty(), "inline chains must describe at least the physical frame");
        assert_eq!(pc, chain[0].pc);
        assert_eq!(iseq, chain[0].iseq);

        let inline_count: u32 = chain.len().try_into().unwrap();
        let inline_frames = Box::leak(chain.into_boxed_slice()).as_ptr();

        let materialize_block_code = !iseq_may_write_block_code(iseq);
        let frame = Self::alloc(pc, iseq, materialize_block_code, stack_size);
        unsafe {
            let frame = frame.cast_mut();
            (*frame).inline_count = inline_count;
            (*frame).sp_offset = sp_offset.try_into().unwrap();
            (*frame).inline_frames = inline_frames;
        }
        frame
    }

    /// The inline chain attached to this JITFrame, if any.
    fn inline_frames_mut(&mut self) -> &mut [InlineFrame] {
        if self.inline_frames.is_null() {
            return &mut [];
        }
        // The chain is uniquely owned by this JITFrame (allocated in
        // new_iseq_with_chain and never shared), so handing out a mutable
        // slice from &mut self is sound despite the *const field type, which
        // only reflects that JIT code and C walkers never write through it.
        unsafe { std::slice::from_raw_parts_mut(self.inline_frames.cast_mut(), self.inline_count as usize) }
    }

    /// Mark the iseq pointer and any inline chain members for GC. Called
    /// from rb_zjit_root_mark.
    pub fn mark(&self) {
        if !self.iseq.is_null() {
            unsafe { rb_gc_mark_movable(VALUE::from(self.iseq)); }
        }

        for i in 0..self.inline_count as usize {
            let vframe = unsafe { &*self.inline_frames.add(i) };
            if !vframe.iseq.is_null() {
                unsafe { rb_gc_mark_movable(VALUE::from(vframe.iseq)); }
            }
            if !vframe.cme.is_null() {
                unsafe { rb_gc_mark_movable(VALUE::from(vframe.cme)); }
            }
            // recv and specval are not marked here: receiver locations are
            // either immediates or native stack slots covered by conservative
            // machine stack marking, and static specvals are either the
            // block-handler-none immediate or a tagged EP kept alive by the
            // bmethod's Proc through the cme marked above.
        }
    }

    /// Update the iseq pointer and any inline chain members after GC
    /// compaction.
    ///
    /// Signal-context frame walkers (rb_profile_frames) may read these
    /// fields concurrently with compaction, so every update must be a single
    /// aligned pointer store, never a read-modify-write. A racing reader
    /// then observes either the old or the new location, both of which
    /// describe the same object. This mirrors the long-standing contract for
    /// the jit_frame->iseq update above.
    pub fn update_references(&mut self) {
        if !self.iseq.is_null() {
            let new_iseq = unsafe { rb_gc_location(VALUE::from(self.iseq)) }.as_iseq();
            if self.iseq != new_iseq {
                self.iseq = new_iseq;
            }
        }

        let self_iseq = self.iseq;
        for vframe in self.inline_frames_mut() {
            if !vframe.iseq.is_null() {
                vframe.iseq = unsafe { rb_gc_location(VALUE::from(vframe.iseq)) }.as_iseq();
            }
            if !vframe.cme.is_null() {
                vframe.cme = unsafe { rb_gc_location(VALUE::from(vframe.cme)) }.as_cme();
            }
        }

        // The innermost chain entry duplicates the JITFrame's iseq, and both
        // are updated from the same old pointer, so they must remain in sync
        // through compaction.
        if let Some(innermost) = self.inline_frames_mut().first() {
            debug_assert_eq!(self_iseq, innermost.iseq);
        }
    }
}

/// Update the iseq pointer in an on-stack JITFrame during GC compaction.
/// Called from rb_execution_context_update in vm.c.
#[unsafe(no_mangle)]
pub extern "C" fn rb_zjit_jit_frame_update_references(jit_frame: *mut JITFrame) {
    unsafe { &mut *jit_frame }.update_references();
}

#[cfg(test)]
mod tests {
    use crate::cruby::{eval, inspect};
    use insta::assert_snapshot;

    #[test]
    fn test_jit_frame_entry_first() {
        eval(r#"
            def test
              itself
              callee
            end

            def callee
              caller
            end

            test
        "#);
        assert_snapshot!(inspect("test.first"), @r#""<compiled>:4:in 'Object#test'""#);
    }

    #[test]
    fn test_materialize_one_frame() {
        assert_snapshot!(inspect("
            def jit_entry
              raise rescue 1
            end
            jit_entry
            jit_entry
        "), @"1");
    }

    #[test]
    fn test_materialize_two_frames() { // materialize caller frames on raise
        // At the point of `rescue`, there are two inline frames on stack and both need to be
        // materialized before passing control to interpreter.
        assert_snapshot!(inspect("
            def jit_entry = raise_and_rescue
            def raise_and_rescue
              raise rescue 1
            end
            jit_entry
            jit_entry
        "), @"1");
    }

    // Direct JIT-to-JIT entry passes callee locals as native arguments. If the
    // callee ISEQ has already escaped EP, later getlocal reads use EP memory,
    // so JIT entry must materialize those locals into the callee frame.
    #[test]
    fn test_jit_entry_materializes_ep_escaped_locals() {
        assert_snapshot!(inspect("
            def poison(*) = nil

            def victim(a, b, c)
              lambda { a }
              a
            end

            def jit_entry
              poison([], [], [], [])
              victim(:expected, 1, 2)
            end

            jit_entry
            Array.new(100) { jit_entry }.uniq
        "), @"[:expected]");
    }

    // Materialize frames on side exit: a type guard triggers a side exit with
    // multiple JIT frames on the stack. All frames must be materialized before
    // the interpreter resumes.
    #[test]
    fn test_side_exit_materialize_frames() {
        assert_snapshot!(inspect("
            def side_exit(n) = 1 + n
            def jit_frame(n) = 1 + side_exit(n)
            def entry(n) = jit_frame(n)
            entry(2)
            [entry(2), entry(2.0)]
        "), @"[4, 4.0]");
    }

    // BOP invalidation must not overwrite the top-most frame's PC with
    // jit_frame's PC. After invalidation the interpreter resumes at a new
    // PC, so a stale jit_frame PC would cause wrong execution.
    #[test]
    fn test_bop_invalidation() {
        assert_snapshot!(inspect(r#"
            def test
              eval("class Integer; def +(_) = 100; end")
              1 + 2
            end
            test
            test
        "#), @"100");
    }

    // Side exit at the very start of a method, before gen_save_pc_for_gc has
    // updated the entry JITFrame.
    #[test]
    fn test_side_exit_before_jit_frame_update() {
        assert_snapshot!(inspect("
            def entry(n) = n + 1
            entry(1)
            [entry(1), entry(1.0)]
        "), @"[2, 2.0]");
    }

    #[test]
    fn test_caller_iseq() {
        assert_snapshot!(inspect(r#"
            def callee = call_caller
            def test = callee

            def callee2 = call_caller
            def test2 = callee2

            def call_caller = caller

            test
            test2
            test.first
        "#), @r#""<compiled>:2:in 'Object#callee'""#);
    }

    // ISEQ must be readable during exception handling so the interpreter
    // can look up rescue/ensure tables.
    #[test]
    fn test_iseq_on_raise() {
        assert_snapshot!(inspect(r#"
            def jit_entry(v) = make_range_then_exit(v)
            def make_range_then_exit(v)
              range = (v..1)
              super rescue range
            end
            jit_entry(0)
            jit_entry(0)
            jit_entry(0/1r)
        "#), @"(0/1)..1");
    }

    // Multiple exception raises during keyword argument evaluation: each
    // raise needs correct ISEQ for catch table lookup.
    #[test]
    fn test_iseq_on_raise_on_ensure() {
        assert_snapshot!(inspect(r#"
            def raise_a = raise "a"
            def raise_b = raise "b"
            def raise_c = raise "c"

            def foo(a: raise_a, b: raise_b, c: raise_c)
              [a, b, c]
            end

            def test_a
              foo(b: 2, c: 3)
            rescue RuntimeError => e
              e.message
            end

            def test_b
              foo(a: 1, c: 3)
            rescue RuntimeError => e
              e.message
            end

            def test_c
              foo(a: 1, b: 2)
            rescue RuntimeError => e
              e.message
            end

            def test
              [test_a, test_b, test_c]
            end

            test
            test
        "#), @r#"["a", "b", "c"]"#);
    }

    // Send fallback (e.g. method_missing) calls into the interpreter, which
    // reads cfp->iseq via GET_ISEQ(). gen_prepare_non_leaf_call writes the
    // iseq to JITFrame, but GET_ISEQ reads cfp->iseq directly. This test
    // ensures the interpreter can resolve the caller iseq for backtraces.
    #[test]
    fn test_send_fallback_caller_location() {
        assert_snapshot!(inspect(r#"
            def callee = caller_locations(1, 1)[0].label
            def test = callee
            test
            test
        "#), @r#""Object#test""#);
    }

    // A send fallback may throw (e.g. via method_missing raising). The
    // interpreter must be able to find the correct rescue handler in the
    // caller's ISEQ catch table. This exercises throw through send fallback.
    #[test]
    fn test_send_fallback_throw() {
        assert_snapshot!(inspect(r#"
            class Foo
              def method_missing(name, *) = raise("no #{name}")
            end
            def test
              Foo.new.bar
            rescue RuntimeError => e
              e.message
            end
            test
            test
        "#), @r#""no bar""#);
    }

    // Proc.new inside a block passed via invokeblock captures the caller's
    // block_code. When the JIT compiles the caller, block_code must be
    // correctly available for the proc to work.
    #[test]
    fn test_proc_from_invokeblock() {
        assert_snapshot!(inspect("
            def capture_block(&blk) = blk
            def test = capture_block { 42 }
            test
            test.call
        "), @"42");
    }

    // binding() called from a JIT-compiled callee must see the correct
    // source location (iseq + pc) of the caller frame.
    #[test]
    fn test_binding_source_location() {
        assert_snapshot!(inspect(r#"
            def callee = binding
            def test = callee
            test
            b = test
            b.source_location[1] > 0
        "#), @"true");
    }

    // $~ (Regexp special variable) is stored via svar which walks the EP
    // chain to find the LEP. rb_vm_svar_lep uses rb_zjit_cfp_has_iseq to
    // skip C frames, so it must work correctly with JITFrame.
    #[test]
    fn test_svar_regexp_match() {
        assert_snapshot!(inspect(r#"
            def test(s)
              s =~ /hello/
              $~
            end
            test("hello world")
            test("hello world").to_s
        "#), @r#""hello""#);
    }

    // C function calls with rb_block_call (like Array#each, Enumerable#map)
    // write an ifunc to cfp->block_code after the JIT pushes the C frame.
    // GC must mark and relocate this ifunc. This test exercises the code
    // path fixed by "Fix ZJIT segfault: write block_code for C frames and
    // fix GC marking".
    #[test]
    fn test_cfunc_block_code_gc() {
        assert_snapshot!(inspect("
            def test
              # Use a cfunc that calls back into Ruby with a block (rb_block_call)
              [1, 2, 3].map { |x| x.to_s }
            end
            test
            test
        "), @r#"["1", "2", "3"]"#);
    }

    // Multiple levels of cfunc-with-block: a JIT-compiled method calls a
    // cfunc that yields, and the block itself calls another cfunc that
    // yields. Each C frame's block_code must be properly initialized.
    #[test]
    fn test_nested_cfunc_with_block() {
        assert_snapshot!(inspect("
            def test
              [1, 2].flat_map { |x| [x, x + 10].map { |y| y * 2 } }
            end
            test
            test
        "), @"[2, 22, 4, 24]");
    }
}
