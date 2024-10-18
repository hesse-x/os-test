#ifndef LIBC_STRINGS_H_
#define LIBC_STRINGS_H_

void int_to_ascii(int n, char str[]);
void reverse(char s[]);
int strlen(char s[]);
void backspace(char s[]);
void append(char s[], char n);
int strcmp(char s1[], char s2[]);

#endif // LIBC_STRINGS_H_
