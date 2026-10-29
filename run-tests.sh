#!/bin/sh
# Regression tests for crepl.  Each case feeds a script on stdin and greps the
# transcript for an expected string.  Exit status is the number of failures.

CREPL=./crepl
pass=0
fail=0

check() {
    name=$1; expect=$2; script=$3
    got=$(printf '%s\n' "$script" | $CREPL 2>&1)
    if printf '%s' "$got" | grep -qF "$expect"; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$name"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n         expected: %s\n' "$name" "$expect"
        printf '%s\n' "$got" | sed 's/^/         | /'
    fi
}

echo "evaluation"
check "integer expression"    "=> 42"        'int x = 41;
x + 1
:quit'
check "string expression"     '=> "hi"'      '"hi"
:quit'
check "double expression"     "=> 7.5"       '15.0 / 2
:quit'
check "char expression"       "=> 'A' (65)"  "char ch = 'A';
ch
:quit"
check "bool expression"       "=> true"      '#include <stdbool.h>
bool b = 1 == 1;
b
:quit'
check "pointer expression"    "=> NULL"      '(void*)0
:quit'
check "function definition"   "=> 120"       'int f(int n){return n<=1?1:n*f(n-1);}
f(5)
:quit'
check "statement"             "i=0 i=1 i=2"  'for (int i=0;i<3;i++) printf("i=%d ", i);
:quit'
check "side effect + value"   "side effect"  'printf("side effect\n");
:quit'
check "struct hexdump"        "=> <8 bytes"  'typedef struct { int x, y; } P;
P p = {3,4};
p
:quit'
check "runtime initialiser"   "=> 7"         'int *a = malloc(4*sizeof(int));
a[0] = 7;
a[0]
:quit'
check "define"                "=> 25"        '#define SQ(x) ((x)*(x))
SQ(5)
:quit'
check "include math"          "=> 4"         '#include <math.h>
(int)sqrt(16.0)
:quit'

echo "classification"
check "assignment is stmt"    "=> 5"         'int v = 0;
v = 5;
:quit'
check "deref not a decl"      "Segmentation" 'int *p = 0;
*p = 1;
:quit'
check "implicit fn rejected"  "implicit declaration" 'nosuchfunc(1)
:quit'
check "int conversion caught" "int-conversion" 'int n = 0;
n = "str";
:quit'

echo "recovery"
check "segfault survives"     "=> 99"        'int keep = 99;
*(int*)0 = 1;
keep
:quit'
check "null deref reported"   "at address (nil)" '*(int*)0 = 1;
:quit'
check "divide by zero"        "=> 1"         '1/0
1
:quit'
check "abort survives"        "=> 2"         'abort();
2
:quit'
check "exit is trapped"       "called exit(3)" 'exit(3);
:quit'
check "alive after exit"      "=> 3"         'exit(7);
3
:quit'
check "timeout fires"         "exceeded the 1 s" ':set timeout 1
while (1) {}
:quit'
check "alive after timeout"   "=> 8"         ':set timeout 1
while (1) {}
8
:quit'

echo "meta commands"
check ":list"                 "1  decl"      'int a = 1;
:list
:quit'
check ":type int"             "int"          'int a = 1;
:type a
:quit'
check ":type double"          "double"       ':type 1.0 + 2
:quit'
check ":type pointer"         "char *"       'char *s = "x";
:type s
:quit'
check ":undo"                 "session is empty" 'int a = 1;
:undo
:list
:quit'
check ":reset"                "session cleared" 'int a = 1;
:reset
:quit'
check ":prologue"             "extern int a;" 'int a = 1;
:prologue
:quit'
check ":decl"                 "=> 3"         ':decl extern int strcmp_dummy;
1 + 2
:quit'
check ":set echo off"         ""             ':set echo off
1 + 1
:quit'
check ":reload keeps state"   "=> 5"         'int c = 0;
c = 5;
:reload
c
:quit'

echo "redefinition"
check "function redefined"    "=> 999"       'int g(void){return 1;}
g()
int g(void){return 999;}
g()
:quit'
check "variable redefined"    "=> 3"         'int z = 1;
int z = 3;
z
:quit'

echo "files"
cat > /tmp/crepl-t1.c <<'EOF'
#include <stdio.h>
typedef struct { int a; } Box;
int box_total = 0;
int box_add(int n) { box_total += n; return box_total; }
const char *box_name(void) { return "box"; }
EOF
check ":load"                 "=> 15"        ':load /tmp/crepl-t1.c
box_add(5)
box_add(10)
:quit'
check ":load derives decls"   "int box_add(int n);" ':load /tmp/crepl-t1.c
:prologue
:quit'
check ":load string fn"       '=> "box"'     ':load /tmp/crepl-t1.c
box_name()
:quit'
cat > /tmp/crepl-t2.c <<'XEOF'
#include <stdio.h>
int main(int argc, char **argv) { printf("argc=%d\n", argc); return argc == 1 ? 0 : 42; }
XEOF
check ":main"                 "main returned 0"  ':load /tmp/crepl-t2.c
:main
:quit'
check ":main with args"       "main returned 42" ':load /tmp/crepl-t2.c
:main a b
:quit'
check ":main not crepl main"  "argc=1"           ':load /tmp/crepl-t2.c
:main
:quit'
check ":main absent"          "no main()"        ':main
:quit'
check ":save"                 "wrote"        'int q = 1;
:save /tmp/crepl-out.c
:quit'
rm -f /tmp/crepl-t1.c /tmp/crepl-t2.c /tmp/crepl-out.c

echo "runtime initialisers"
check "strdup binding"        '=> "hello"'   'char *buf = strdup("hello");
buf
:quit'
check "chained initialiser"   "=> 6"         'int m = 3, n = m * 2;
n
:quit'
check "array initialiser"     "=> 2"         'int arr[3] = {1,2,3};
arr[1]
:quit'

echo "multi-line"
check "brace continuation"    "=> 21"        'int gcd(int a, int b) {
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}
gcd(1071, 462)
:quit'
check "explicit block"        "=> 6765"      ':{
int fib(int n)
{
    if (n < 2) return n;
    return fib(n-1) + fib(n-2);
}
:}
fib(20)
:quit'
check "backslash join"        "=> 30"        'int w = 10 + \
20;
w
:quit'

printf '\n%d passed, %d failed\n' "$pass" "$fail"
exit $fail
