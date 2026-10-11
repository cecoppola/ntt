cd ~/ntt-wt/c06/ecalc; source aac7env.sh >/dev/null 2>&1
LINE=$(bash -c 'unset E16_EXTRA; eval "$(grep -m1 "^LINE=" "$1")"; echo $LINE' _ ./e16_headline.sh)
for g in 575 574 573 572 571 570 569 568 567 566 565 564 563 562 561 560; do
  MG=2,4,8,16,32,64,192,$g
  D=$(python3 -c "print(int(6.441e10*$g))")
  echo "== g $g"
  env $LINE COMM_OFI_PLAN_CXI=1 MN_GROUPS=$MG MN_PLAN_ONLY=$D:$g ./ecalc $D /dev/null 2>&1 | grep -E "^plan (check|vslot-prealloc|pool|vslot )" | sed -E 's/\(S31.*//; s/^plan vslot  .*per APU from the dm phase/vslot per APU from dm/; s/the SHMEM pool per node-process: //' | cut -c1-210
done
