#ifndef _CTYPE_H
#define _CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int isspace(int c);
int ispunct(int c);
int islower(int c);
int isupper(int c);
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
}
#endif

#endif /* _CTYPE_H */
