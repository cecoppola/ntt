#pragma once
/* config_line.c -- print-only "effective configuration" line. cfg_snapshot(): at the top of main, before ecalc sets
 * any variable, record which registered switches (switches_gen.h) are set. cfg_print(eff): print  config: hash=<8
 * hex> set=<k=v ...> eff=<resolved values>  (rank 0 only; hash = FNV-1a 32 over the sorted "k=v\n" lines of the set
 * switches). */
int cfg_guard(void);               /* 1 (after naming each on stderr) if the environment has a name with one of our prefixes that is not registered */
void cfg_settings(void);           /* `ecalc --settings`: every registered setting, its default, and the value in force */
void cfg_snapshot(void);
void cfg_print(const char *eff);
