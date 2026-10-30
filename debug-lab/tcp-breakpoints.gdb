set pagination off
set breakpoint pending on
set print pretty on
set $seen_put = 0
set $seen_submit = 0
set $seen_read = 0
set $seen_write = 0
break mooncake::Client::Put
commands
  silent
  printf "STORE_PUT_BREAKPOINT\n"
  set $seen_put = 1
  print key
  continue
end
break mooncake::TcpTransport::submitTransferTask
commands
  silent
  printf "TCP_SUBMIT_BREAKPOINT\n"
  set $seen_submit = 1
  bt 10
  continue
end
break mooncake::TcpTransport::startTransfer
commands
  silent
  printf "TCP_START_BREAKPOINT\n"
  print *slice
  if slice->opcode == 0
    set $seen_read = 1
  else
    set $seen_write = 1
  end
  continue
end
run roundtrip
if $_exitcode != 0 || !$seen_put || !$seen_submit || !$seen_read || !$seen_write
  printf "GDB_TCP_FAILED\n"
  quit 1
end
printf "GDB_TCP_OK: Store Put, TCP WRITE, TCP READ, locals and stacks\n"
quit 0
