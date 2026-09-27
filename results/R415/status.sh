#!/bin/bash
# R4: the chain's progress lines
cat $HOME/R415/chain.log
for b in A B C D; do
  [ -f $HOME/R415/$b/driver.log ] && grep -aE "^(job|running|commit|WRONG|==|rc|done|gone|SKIP)|cmp |sha |wall |PASS|FAIL|VERIFY" $HOME/R415/$b/driver.log | sed "s/^/$b: /"
done
