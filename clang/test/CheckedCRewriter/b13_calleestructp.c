// RUN: cconv-standalone %s -- | FileCheck -match-full-lines %s

struct np {
  int x;
  int y;
};

struct p {
  int *x;
  char *y;
};
//CHECK: int *x;

struct r {
  int data;
  struct r *next;
};
//CHECK: _Ptr<struct r> next;

struct p sus(struct p x) {
  x.x += 1;
  struct p *n = malloc(sizeof(struct p));
  return *n;
}
//CHECK: _Ptr<struct p> n =  malloc(sizeof(struct p));

struct p foo() {
  struct p x;
  struct p z = sus(x);
  return z;
}
//CHECK: struct p x = {};

struct p bar() {
  struct p x;
  struct p z = sus(x);
  return z;
}
