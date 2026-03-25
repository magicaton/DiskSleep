; x64 stack probe for noCRT builds.
; LTCG ignores /Gs (StackProbeSize), so the compiler may emit __chkstk calls
; for large stack allocations.  This provides the required implementation.
;
; __chkstk receives the requested allocation size in RAX and probes every
; 4 KB page from the current stack pointer down to the target address.

_TEXT SEGMENT

PUBLIC __chkstk

__chkstk PROC
        push    rcx
        push    rax
        cmp     rax, 1000h
        lea     rcx, [rsp + 18h]       ; original RSP (skip saved rcx + rax + return addr)
        jb      last_page

probe_loop:
        sub     rcx, 1000h
        test    dword ptr [rcx], eax   ; touch the page
        sub     rax, 1000h
        cmp     rax, 1000h
        ja      probe_loop

last_page:
        sub     rcx, rax
        test    dword ptr [rcx], eax   ; touch the final page
        pop     rax
        pop     rcx
        ret

__chkstk ENDP

_TEXT ENDS
END
