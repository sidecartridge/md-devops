; SidecarTridge Multi-device shared functions

; constants
_p_cookies                              equ $5a0    ; pointer to the system Cookie-Jar

COOKIE_JAR_MEGASTE                      equ $00010010 ; Mega STE computer
MEGASTE_SPEED_CACHE_REG                 equ $FFFF8E21 ; Mega STE: bit 0 the cache, bit 1 16 MHz
SHARED_VARIABLE_SHARED_FUNCTIONS_SIZE   equ 16      ; Size of the shared variables for the shared functions
SHARED_VARIABLE_HARDWARE_TYPE           equ 0       ; Hardware type of the Atari ST computer

COMMAND_SYNC_CODE_SIZE                  equ (4 + _end_sync_code_in_stack - _start_sync_code_in_stack)
COMMAND_SYNC_WRITE_CODE_SIZE            equ (4 + _end_sync_write_code_in_stack - _start_sync_write_code_in_stack)

COMMAND_SYNC_USE_DSKBUF                 equ 0       ; Use different disk buffers to store the sync wait code
                                                    ; 2: Use the stack to store the code. Dangerous
                                                    ; 1: Use the disk buffer to store the code. Safe
                                                    ; 0: Use the ROM address to store the code. Safe


; Send an sync command to the Sidecart
; Wait until the command sets a response in the memory with a random number used as a token
; Input registers:
; d0.w: command code
; d1.w: payload size
; From d3 to d6 the payload based on the size of the payload field d1.w
; Output registers:
; d0: error code, 0 if no error
; d1-d7 are modified. a0-a3 modified.
send_sync_command_to_sidecart:
    ; The random token is used to synchronize with the sidecart
    move.l RANDOM_TOKEN_SEED_ADDR, d2

    ifne COMMAND_SYNC_USE_DSKBUF == 1   ; Use the disk buffer to store the code
        move.l _dskbufp.w, a2
        move.l _dskbufp.w, a3
    else
        ifne COMMAND_SYNC_USE_DSKBUF == 2   ; Use the stack to store the code
            lea -(256)(sp), a2
            move.l a2, a3
        else
            ; No need to copy the code to the stack, we can use the ROM address
        endif
    endif

    ifne COMMAND_SYNC_USE_DSKBUF != 0   ; Copy the code for stack and disk buffer
        move.l #COMMAND_SYNC_CODE_SIZE, d7
        lea _start_sync_code_in_stack, a1    ; a1 points to the start of the code in ROM
        lsr.w #1, d7
        subq #1, d7
_copy_sync_code:
        move.w (a1)+, (a2)+
        dbf d7, _copy_sync_code
    endif

    ; The sync command synchronize with a random token
    addq.w #4, d1                 ; Add 4 bytes to the payload size to include the token

    ; a1 points to the addres of the token modified by the rp2040
    lea RANDOM_TOKEN_ADDR, a1

    ; For performance reasons, we will positive and negative index values to avoid some operations
    move.l #ROMCMD_START_ADDR, a0 ; Start address of the ROM3
    add.l #$8000, a0              ; Add 32Kb to the address to point to the middle of the ROM

    ; SEND HEADER WITH MAGIC NUMBER
    move.w #CMD_MAGIC_NUMBER, d7 ; Command header
    tst.b (a0, d7.w)             ; Command header

    ; Clean the CHECKSUM register in d7
    clr.l d7

    ; SEND COMMAND CODE
    add.w d0, d7                ; Add the payload size to the checksum
    tst.b (a0, d0.w)            ; Command code. d0 is a scratch register

    ; SEND PAYLOAD SIZE
    add.w d1, d7                ; Add the payload size to the checksum
    tst.b (a0, d1.w)
    tst.w d1
    beq _no_more_payload_stack  ; If the command does not have payload, we are done.

    ; SEND PAYLOAD LOW D2
    add.w d2, d7                ; Add the payload to the checksum
    tst.b (a0, d2.w)
    cmp.w #2, d1
    beq _no_more_payload_stack

    ; SEND PAYLOAD HIGH D2
    swap d2
    add.w d2, d7              ; Add the payload to the checksum
    tst.b (a0, d2.w)
    cmp.w #4, d1
    beq _no_more_payload_stack

    ; SEND PAYLOAD LOW D3
    add.w d3, d7              ; Add the payload to the checksum
    tst.b (a0, d3.w)
    cmp.w #6, d1
    beq _no_more_payload_stack

    ; SEND PAYLOAD HIGH D3
    swap d3
    add.w d3, d7              ; Add the payload to the checksum
    tst.b (a0, d3.w)
    cmp.w #8, d1
    beq _no_more_payload_stack

    ; SEND PAYLOAD LOW D4
    add.w d4, d7              ; Add the payload to the checksum
    tst.b (a0, d4.w)
    cmp.w #10, d1
    beq _no_more_payload_stack

    ; SEND PAYLOAD HIGH D4
    swap d4
    add.w d4, d7              ; Add the payload to the checksum
    tst.b (a0, d4.w)
    cmp.w #12, d1
    beq.s _no_more_payload_stack

    ; SEND PAYLOAD LOW D5
    add.w d5, d7              ; Add the payload to the checksum
    tst.b (a0, d5.w)
    cmp.w #14, d1
    beq.s _no_more_payload_stack

    ; SEND PAYLOAD HIGH D5
    swap d5
    add.w d5, d7              ; Add the payload to the checksum
    tst.b (a0, d5.w)
    cmp.w #16, d1
    beq.s _no_more_payload_stack

    ; SEND PAYLOAD LOW D6
    add.w d6, d7              ; Add the payload to the checksum
    tst.b (a0, d6.w)
    cmp.w #18, d1
    beq.s _no_more_payload_stack

    ; SEND PAYLOAD HIGH D6
    swap d6
    add.w d6, d7              ; Add the payload to the checksum
    tst.b (a0, d6.w)

_no_more_payload_stack:
    ; SEND CHECKSUM
    tst.b (a0, d7.w)
    ifne COMMAND_SYNC_USE_DSKBUF !=0 ; If we are copying the code, we have to jump there
        jmp (a3)                    ; Jump to the code in the stack
    endif
; This is the code that cannot run in ROM while waiting for the command to complete
_start_sync_code_in_stack:
    ; End of the command loop. Now we need to wait for the token
    swap d2                                  ; D2 is the only register that is not used as a scratch register
    move.l #COMMAND_TIMEOUT, d7              ; Most significant word is the inner loop, least significant word is the outer loop
    moveq #0, d0                             ; No Timeout
_start_sync_code_in_stack_loop:
    cmp.l (a1), d2                           ; Compare the random number with the token
    bne.s _sync_token_not_ready
    cmp.l RANDOM_TOKEN_SEED_ADDR, d2         ; Seed must advance to prove a real response
    bne.s _sync_token_found                  ; Token found, we can finish succesfully
_sync_token_not_ready:
    subq.l #1, d7                            ; Decrement the inner loop
    bne.s _start_sync_code_in_stack_loop     ; If the inner loop is not finished, continue

    ; Sync token not found, timeout
    subq.l #1, d0                            ; Timeout
_sync_token_found:

;    move.l #RANDOM_TOKEN_POST_WAIT, d7
;_postwait_me:
;    dbf d7, _postwait_me
;_no_wait_me:
    rts                                 ; Return to the code
_end_sync_code_in_stack:

; Send an sync write command to the Sidecart
; Wait until the command sets a response in the memory with a random number used as a token
; Input registers:
; d0.w: command code
; d3.l: long word to send to the sidecart
; d4.l: long word to send to the sidecart
; d5.l: long word to send to the sidecart
; d6.w: number of bytes to write to the sidecart starting in a4 address
; a4: address of the buffer to write in the sidecart
; Output registers:
; d0: error code, 0 if no error
; d7: 16 bit checksum of the data written from address from a4 to a4 + d6
; a4: next address in the computer memory to retrieve
; d1-d6 are modified. a0-a3 modified.
send_sync_write_command_to_sidecart:
    ; The random token is used to synchronize with the sidecart
    move.l RANDOM_TOKEN_SEED_ADDR, d2

    ifne COMMAND_SYNC_USE_DSKBUF == 1   ; Use the disk buffer to store the code
        move.l _dskbufp.w, a2
        move.l _dskbufp.w, a3
    else
        ifne COMMAND_SYNC_USE_DSKBUF == 2   ; Use the stack to store the code
            lea -(256)(sp), a2
            move.l a2, a3
        else
            ; No need to copy the code to the stack, we can use the ROM address
        endif
    endif

    ifne COMMAND_SYNC_USE_DSKBUF != 0   ; Copy the code for stack and disk buffer
        move.l #COMMAND_SYNC_WRITE_CODE_SIZE, d7
        lea _start_sync_write_code_in_stack, a1    ; a1 points to the start of the code in ROM
        lsr.w #1, d7
        subq #1, d7
_copy_sync_code_write:
        move.w (a1)+, (a2)+
        dbf d7, _copy_sync_code_write
    endif

; Adjust the payload size to include the buffer
    and.l #$FFFF, d6                    ; Remove the upper word of the payload size. Only 65536 bytes are allowed
    moveq.l #16, d1                     ; We are going to send the data in d2.l (random token), d3.l, d4.l and d5.l. ALWAYS!!!!
    add.l d6, d1                        ; Add the number of bytes to write to the sidecart
    addq.l #1, d1                       ; Add one byte to the payload before rounding to the next word
    lsr.l #1, d1                        ; Round to the next word
    lsl.l #1, d1                        ; Multiply by 2 because we are sending two bytes each iteration

    ; a1 points to the addres of the token modified by the rp2040
    lea RANDOM_TOKEN_ADDR, a1

    ; For performance reasons, we will positive and negative index values to avoid some operations
    move.l #ROMCMD_START_ADDR, a0 ; Start address of the ROM3
    add.l #$8000, a0              ; Add 32Kb to the address to point to the middle of the ROM

     ; SEND HEADER WITH MAGIC NUMBER
    move.w #CMD_MAGIC_NUMBER, d7 ; Command header
    tst.b (a0, d7.w)             ; Command header

    ; Clean the CHECKSUM register in d7
    clr.l d7

    ; SEND COMMAND CODE
    add.w d0, d7                ; Add the payload size to the checksum
    tst.b (a0, d0.w)            ; Command code. d0 is a scratch register

    ; SEND PAYLOAD SIZE
    add.w d1, d7                ; Add the payload size to the checksum
    tst.b (a0, d1.w)

    ; SEND PAYLOAD LOW D2
    add.w d2, d7                ; Add the payload to the checksum
    tst.b (a0, d2.w)

    ; SEND PAYLOAD HIGH D2
    swap d2
    add.w d2, d7              ; Add the payload to the checksum
    tst.b (a0, d2.w)

    ; SEND PAYLOAD LOW D3
    add.w d3, d7              ; Add the payload to the checksum
    tst.b (a0, d3.w)

    ; SEND PAYLOAD HIGH D3
    swap d3
    add.w d3, d7              ; Add the payload to the checksum
    tst.b (a0, d3.w)

    ; SEND PAYLOAD LOW D4
    add.w d4, d7              ; Add the payload to the checksum
    tst.b (a0, d4.w)

    ; SEND PAYLOAD HIGH D4
    swap d4
    add.w d4, d7              ; Add the payload to the checksum
    tst.b (a0, d4.w)

    ; SEND PAYLOAD LOW D5
    add.w d5, d7              ; Add the payload to the checksum
    tst.b (a0, d5.w)

    ; SEND PAYLOAD HIGH D5
    swap d5
    add.w d5, d7              ; Add the payload to the checksum
    tst.b (a0, d5.w)

    ;
    ; SEND MEMORY BUFFER TO WRITE
    ;
    move.l d6, d5
    move.l d7, d6
    clr.l d7

    btst #0, d5             ; Test if number of bytes to copy is even or odd
    bne.s _write_to_sidecart_byteodd_loop
    addq.l #1, d5            ; Add one byte to the payload before rounding to the next word
    lsr.w #1, d5             ; Copy two bytes each iteration
    subq.w #1, d5            ; one less

    ; Test if the address in A4 is even or odd
    move.l a4, d4
    btst #0, d4
    beq.s _write_to_sidecart_even_loop
_write_to_sidecart_odd_loop:
    move.b  (a4)+, d3       ; Load the high byte
    lsl.w   #8, d3          ; Shift it to the high part of the word
    move.b  (a4)+, d3       ; Load the low byte
    tst.b (a0, d3.w)        ; Write the memory to the sidecart
    add.w d3, d7            ; Add the word to the checksum
    dbf d5, _write_to_sidecart_odd_loop
    bra.s _no_more_payload_write_stack

 _write_to_sidecart_even_loop:
    move.w (a4)+, d4          ; Load the word
    add.w d4, d7              ; Add the word to the checksum
    tst.b (a0, d4.w)          ; Write the memory to the sidecart
    dbf d5, _write_to_sidecart_even_loop
    bra.s _no_more_payload_write_stack

 _write_to_sidecart_byteodd_loop:
    addq.l #1, d5             ; Add one byte to the payload before rounding to the next word
    lsr.w #1, d5              ; Copy two bytes each iteration
    move.l a4, d4             ; Test if the address in A4 is even or odd
    btst #0, d4
    beq.s _write_to_sidecart_byteodd_odd_loop

    subq.w #1, d5
    beq.s _write_to_sidecart_byteodd_loop_tail
    subq.w #1, d5            ; one less
_write_to_sidecart_byteodd_even_loop_copy:
    move.b  (a4)+, d3       ; Load the high byte
    lsl.w   #8, d3          ; Shift it to the high part of the word
    move.b  (a4)+, d3       ; Load the low byte
    tst.b (a0, d3.w)        ; Write the memory to the sidecart
    add.w d3, d7            ; Add the word to the checksum
    dbf d5, _write_to_sidecart_byteodd_even_loop_copy
_write_to_sidecart_byteodd_loop_tail:
    move.b (a4)+, d4          ; Load the high byte
    lsl.w   #8, d4            ; Shift it to the high part of the word
    and.w #$FF00,d4           ; Mask the upper word
    add.w d4, d7              ; Add the word to the checksum
    tst.b (a0, d4.w)          ; Write the memory to the sidecart
    bra.s _no_more_payload_write_stack

_write_to_sidecart_byteodd_odd_loop:
    subq.w #1, d5
    beq.s _write_to_sidecart_byteodd_odd_loop_tail
    subq.w #1, d5            ; one less
_write_to_sidecart_byteodd_odd_loop_copy:
    move.w (a4)+, d4          ; Load the word
    add.w d4, d7              ; Add the word to the checksum
    tst.b (a0, d4.w)          ; Write the memory to the sidecart
    dbf d5, _write_to_sidecart_byteodd_odd_loop_copy
_write_to_sidecart_byteodd_odd_loop_tail:
    move.w (a4)+, d4          ; Load the word
    and.w #$FF00,d4           ; Mask the upper word
    add.w d4, d7              ; Add the word to the checksum
    tst.b (a0, d4.w)          ; Write the memory to the sidecart

_no_more_payload_write_stack:
    add.w d7, d6              ; Add the checksum parameters to the buffer 
    ; SEND CHECKSUM
    tst.b (a0, d6.w)
    ifne COMMAND_SYNC_USE_DSKBUF !=0 ; If we are copying the code, we have to jump there
        jmp (a3)                    ; Jump to the code in the stack
    endif
; This is the code that cannot run in ROM while waiting for the command to complete
_start_sync_write_code_in_stack:
    swap d2                                        ; D2 is the only register that is not used as a scratch register
    move.l #COMMAND_WRITE_TIMEOUT, d6              ; Most significant word is the inner loop, least significant word is the outer loop
    moveq #0, d0                                   ; Timeout
_start_sync_write_code_in_stack_loop:
    cmp.l (a1), d2                                 ; Compare the random number with the token
    bne.s _sync_write_token_not_ready
    cmp.l RANDOM_TOKEN_SEED_ADDR, d2               ; Seed must advance to prove a real response
    bne.s _sync_write_token_found                  ; Token found, we can finish succesfully
_sync_write_token_not_ready:
    subq.l #1, d6                                  ; Decrement the inner loop
    bne.s _start_sync_write_code_in_stack_loop     ; If the inner loop is not finished, continue

    ; Sync token not found, timeout
    subq.l #1, d0                                  ; Timeout

_sync_write_token_found:
;    move.l #RANDOM_TOKEN_POST_WAIT, d6
;_postwait_write_me:
;    dbf d6, _postwait_write_me
;_no_wait_write_me:
    rts                                 ; Return to the code

_end_sync_write_code_in_stack:
