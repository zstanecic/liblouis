/* liblouis Braille Translation and Back-Translation Library

   Copyright (C) 2015 Bert Frees <bertfrees@gmail.com>

   This file is part of liblouis.

   liblouis is free software: you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published
   by the Free Software Foundation, either version 2.1 of the License, or
   (at your option) any later version.

   liblouis is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with liblouis. If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file
 * @brief Find translation tables
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _MSC_VER
#include <windows.h>
#else
#include <dirent.h>
#endif
#include <sys/stat.h>
#include "internal.h"

/* =============================== LIST =================================== */

typedef struct List {
	void *head;
	void (*free)(void *);  // free head
	void *(*dup)(void *);  // dup head
	struct List *tail;
} List;

/**
 * Returns a list with the element `x' added to `list'. Returns a sorted list
 * if `cmp' is not NULL and if `list' is also sorted. New elements replace
 * existing ones if they are equal according to `cmp'. If `cmp' is NULL,
 * elements are simply prepended to the list. The function `dup' is used to
 * duplicate elements before they are added to the list. The `free' function
 * is used to free elements when they are removed from the list. The returned
 * list must be freed by the caller.
 */
static List *
list_conj(List *list, void *x, int (*cmp)(void *, void *), void *(*dup)(void *),
		void (*free)(void *)) {
	if (!list) {
		list = malloc(sizeof(List));
		list->head = dup ? dup(x) : x;
		list->free = free;
		list->dup = dup;
		list->tail = NULL;
		return list;
	} else if (!cmp) {
		List *l = malloc(sizeof(List));
		l->head = dup ? dup(x) : x;
		l->free = free;
		l->dup = dup;
		l->tail = list;
		return l;
	} else {
		List *l1 = list;
		List *l2 = NULL;
		while (l1) {
			int c = cmp(l1->head, x);
			if (c > 0)
				break;
			else if (c < 0) {
				l2 = l1;
				l1 = l2->tail;
			} else {
				if (x != l1->head && !dup && free) free(x);
				return list;
			}
		}
		List *l3 = malloc(sizeof(List));
		l3->head = dup ? dup(x) : x;
		l3->free = free;
		l3->dup = dup;
		l3->tail = l1;
		if (!l2)
			list = l3;
		else
			l2->tail = l3;
		return list;
	}
}

/**
 * Free an instance of type List.
 */
static void
list_free(List *list) {
	if (list) {
		if (list->free) list->free(list->head);
		list_free(list->tail);
		free(list);
	}
}

/**
 * Duplicate an instance of type List.
 */
static List *
list_dup(List *list) {
	if (!list) return list;
	List *d = malloc(sizeof(List));
	d->head = list->dup ? list->dup(list->head) : list->head;
	d->free = list->free;
	d->dup = list->dup;
	d->tail = list_dup(list->tail);
	return d;
}

/**
 * Sort a list based on a comparison function.
 */
static List *
list_sort(List *list, int (*cmp)(void *, void *)) {
	List *newList = NULL;
	List *l;
	for (l = list; l; l = l->tail) {
		newList = list_conj(newList, l->head, cmp, NULL, l->free);
		l->free = NULL;
	}
	list_free(list);
	return newList;
}

/**
 * Get the size of a list.
 */
static int
list_size(List *list) {
	int len = 0;
	List *l;
	for (l = list; l; l = l->tail) len++;
	return len;
}

/**
 * Convert a list into a NULL terminated array.
 */
static void **
list_toArray(List *list, void *(*dup)(void *)) {
	void **array;
	List *l;
	int i;
	array = malloc((1 + list_size(list)) * sizeof(void *));
	i = 0;
	for (l = list; l; l = l->tail) array[i++] = dup ? dup(l->head) : l->head;
	array[i] = NULL;
	return array;
}

/* ============================== FEATURE ================================= */

typedef struct {
	char *key;
	void *val;
	void (*free)(void *);  // free val
	void *(*dup)(void *);  // dup val
} Feature;

typedef struct {
	Feature feature;
	int importance;
} FeatureWithImportance;

typedef struct {
	Feature feature;
	int lineNumber;	 // no line number (-1) means it is a default value
} FeatureWithLineNumber;

typedef struct {
	char *name;
	List *features;
} TableMeta;

/**
 * Create an instance of type Feature.
 *
 * The `key' string is duplicated. What happens with `val' is
 * determined by the `dup' and `free' arguments.
 */
static Feature
feat_new(const char *key, void *val, void *(*dup)(void *), void (*free)(void *)) {
	Feature f;
	f.key = strdup(key);
	f.val = dup ? dup(val) : val;
	f.dup = dup;
	f.free = free;
	return f;
}

/**
 * Free an instance of type Feature
 */
static void
feat_free(Feature *f) {
	if (f) {
		free(f->key);
		if (f->free) f->free(f->val);
		free(f);
	}
}

/**
 * Duplicate an instance of type Feature.
 */
static Feature *
feat_dup(Feature *f) {
	if (!f) return NULL;
	Feature *d = malloc(sizeof(Feature));
	d->key = strdup(f->key);
	d->val = f->dup ? f->dup(f->val) : f->val;
	d->free = f->free;
	d->dup = f->dup;
	return d;
}

/* =========================== LANGUAGE TAGS ============================== */

/**
 * Return true if the tag we're parsing is a language tag (language, region or
 * locale).
 */
static int
isLanguageTag(const char *key, int len) {
	return strncasecmp("language", key, len) == 0 ||
			strncasecmp("region", key, len) == 0 || strncasecmp("locale", key, len) == 0;
}

static int
isAlpha(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int
isAlphaNum(char c) {
	return (c >= '0' && c <= '9') || isAlpha(c);
}

/**
 * Parse language tag into a list of subtags.
 */
static List *
parseLanguageTag(const char *val) {
	List *list = NULL;
	List **tail = &list;
	static char subtag[9];
	if (!*val) return NULL;
	if (val[0] == '*') {
		if (val[1] && val[1] != '-') return NULL;
		*subtag = '\0';
		strncat(subtag, val, 1);
		*tail = list_conj(NULL, subtag, NULL, (void *(*)(void *))strdup, free);
		tail = &(*tail)->tail;
		if (!val[1]) return list;
		val = &val[2];
	}
	while (1) {
		int len = 0;
		for (; len <= 8; len++)
			if (!val[len] || !isAlphaNum(val[len]) || (!list && !isAlpha(val[len])))
				break;
		if (len < 1 || len > 8) return NULL;
		if (val[len] && val[len] != '-') return NULL;
		*subtag = '\0';
		strncat(subtag, val, len);
		*tail = list_conj(NULL, subtag, NULL, (void *(*)(void *))strdup, free);
		tail = &(*tail)->tail;
		if (!val[len]) return list;
		val = &val[len + 1];
	}
	return NULL;
}

/**
 * Serialize language tag.
 *
 * The returned string must be freed by the caller.
 */
static char *
serializeLanguageTag(const List *tag) {
	int len = 0;
	const List *l;
	for (l = tag; l; l = l->tail) len = len + 1 + strlen(l->head);
	char *s = malloc(len * sizeof(char));
	s[0] = '\0';
	for (l = tag; l; l = l->tail) {
		if (l != tag) s = strcat(s, "-");
		s = strcat(s, l->head);
	}
	return s;
}

/* ======================================================================== */

/**
 * Sort features by their key (alphabetical order).
 */
static int
cmpKeys(Feature *f1, Feature *f2) {
	return strcasecmp(f1->key, f2->key);
}

/**
 * Sort features by their key and value (alphabetical order).
 */
static int
cmpFeatures(Feature *f1, Feature *f2) {
	int r = strcasecmp(f1->key, f2->key);
	if (r != 0) return r;
	if (isLanguageTag(f1->key, MAXSTRING)) {
		List *l1 = f1->val;
		List *l2 = f2->val;
		while (l1 && l2) {
			r = strcasecmp(l1->head, l2->head);
			if (r != 0) return r;
			l1 = l1->tail;
			l2 = l2->tail;
		}
		return l1 ? 1 : l2 ? -1 : 0;
	} else
		return strcasecmp(f1->val, f2->val);
}

/**
 * Return a positive number if the given language tag matches the language range,
 * 0 otherwise.
 *
 * In case of a perfect match, return 10. Otherwise, for each extra subtag that
 * has no exact match in the range, subtract two.
 *
 * See also <https://datatracker.ietf.org/doc/html/rfc4647#section-3.3.2>
 */
static int
matchLanguageTags(const List *tag, const List *range) {
	static const int POS_MATCH = 10;
	static const int EXTRA = -2;
	int q = POS_MATCH;
	if (*((char *)range->head) == '*')
		q += EXTRA;
	else if (strcasecmp(tag->head, range->head) != 0)
		return 0;
	range = range->tail;
	tag = tag->tail;
	while (range) {
		if (!tag) return 0;
		if (strcasecmp(tag->head, range->head) == 0) {
			range = range->tail;
			tag = tag->tail;
			continue;
		} else if (strlen(tag->head) == 1)
			return 0;
		else
			q += EXTRA;
		tag = tag->tail;
	}
	while (tag) {
		q += EXTRA;
		tag = tag->tail;
	}
	return q;
}

/**
 * Compute the match quotient of the features in a query against the features in a table's
 * metadata.
 *
 * The features are assumed to be sorted. The query's features must be
 * of type FeatureWithImportance and are assumed to have no duplicate
 * keys. How a feature contributes to the match quotient depends on
 * its importance, on whether the feature is undefined, defined with
 * the same value (positive match), or defined with a different value
 * (negative match), and on the `fuzzy' argument. If the `fuzzy'
 * argument evaluates to true, negative matches and undefined features
 * get a lower penalty.
 */
static int
matchFeatureLists(const List *query, const List *tableFeatures, int fuzzy) {
	static const int POS_MATCH = 10;
	static const int NEG_MATCH = -100;
	static const int UNDEFINED = -20;
	static const int EXTRA = -1;
	static const int POS_MATCH_FUZZY = 10;
	static const int NEG_MATCH_FUZZY = -25;
	static const int UNDEFINED_FUZZY = -5;
	static const int EXTRA_FUZZY = -1;
	int posMatch, negMatch, undefined, extra;
	if (!fuzzy) {
		posMatch = POS_MATCH;
		negMatch = NEG_MATCH;
		undefined = UNDEFINED;
		extra = EXTRA;
	} else {
		posMatch = POS_MATCH_FUZZY;
		negMatch = NEG_MATCH_FUZZY;
		undefined = UNDEFINED_FUZZY;
		extra = EXTRA_FUZZY;
	}
	int quotient = 0;
	const List *l1 = query;
	const List *l2 = tableFeatures;
	while (1) {
		if (!l1) {
			if (!l2) break;
			quotient += extra;
			const List *l = l2;
			l = l->tail;
			while (l && cmpKeys(l->head, l2->head) == 0) l = l->tail;
			l2 = l;
		} else if (!l2) {
			quotient += undefined;
			l1 = l1->tail;
		} else {
			int cmp = cmpKeys(l1->head, l2->head);
			if (cmp < 0) {
				quotient += undefined;
				l1 = l1->tail;
			} else if (cmp > 0) {
				quotient += extra;
				const List *l = l2;
				l = l->tail;
				while (l && cmpKeys(l->head, l2->head) == 0) l = l->tail;
				l2 = l;
			} else {
				const List *l = l2;
				char *k = ((Feature *)l->head)->key;
				int best = negMatch;
				if (isLanguageTag(k, MAXSTRING)) {
					int extraLanguages = 0;
					while (1) {
						// special handling of language tags: tags in the
						// table are intepreted as language ranges
						List *v = ((Feature *)l->head)->val;
						List *v1 = ((Feature *)l1->head)->val;
						int q = matchLanguageTags(v1, v);
						if (q > 0 && q > best)
							best = q;
						else if (!q)
							extraLanguages += extra;
						l = l->tail;
						if (!l || cmpKeys(l->head, l2->head) != 0) break;
					}
					if (best > 0)
						best += ((extraLanguages + 4) /
								5);	 // penalty for extra languages is lower than penalty
									 // for fields that are not in query at all
				} else {
					while (1) {
						if (best < 0) {
							char *v = ((Feature *)l->head)->val;
							char *v1 = ((Feature *)l1->head)->val;
							if (strcasecmp(v1, v) == 0)
								best = posMatch;
							else if (strcasecmp(k, "unicode-range") == 0) {
								// special handling of unicode-range: ucs2 in
								// table also matches ucs4 in query
								if (strcasecmp(v1, "ucs4") == 0 &&
										strcasecmp(v, "ucs2") == 0) {
									best = posMatch;
									best--;	 // add small penalty to favour ucs4 table
											 // if it exists
								}
							}
						}
						l = l->tail;
						if (!l || cmpKeys(l->head, l2->head) != 0) break;
					}
				}
				quotient += best;
				l1 = l1->tail;
				l2 = l;
			}
		}
	}
	return quotient;
}

/**
 * Return true if a character matches [0-9A-Za-z_-\.]
 */
static int
isValidChar(char c) {
	return isAlphaNum(c) || c == '-' || c == '.' || c == '_';
}

/**
 * Return true if a character matches [\s\t]
 */
static int
isSpace(char c) {
	return c == ' ' || c == '\t';
}

/**
 * Parse a table query into a list of features. Features defined first get a
 * higher importance.
 */
static List *
parseQuery(const char *query) {
	List *features = NULL;
	const char *key = NULL;
	const char *val = NULL;
	size_t keySize = 0;
	size_t valSize = 0;
	const char *c;
	int pos = 0;
	int unicodeRange = 0;
	while (1) {
		c = &query[pos++];
		if (isSpace(*c) || (*c == '\n') || (*c == '\0')) {
			if (key) {
				char *v = NULL;
				if (val) {
					v = malloc(valSize + 1);
					v[valSize] = '\0';
					memcpy(v, val, valSize);
				}
				if (!v) goto compile_error;
				char *k = malloc(keySize + 1);
				k[keySize] = '\0';
				memcpy(k, key, keySize);
				if (isLanguageTag(k, keySize)) {
					List *tag = parseLanguageTag(v);
					if (!tag) {
						_lou_logMessage(LOU_LOG_ERROR, "Not a valid language tag: %s", v);
						free(k);
						free(v);
						list_free(features);
						return NULL;
					}
					if (strcasecmp(k, "locale") == 0) {
						// locale is shorthand for language + region
						FeatureWithImportance f1 = { feat_new("language", tag, NULL,
															 (void (*)(void *))list_free),
							0 };
						FeatureWithImportance f2 = { feat_new("region", list_dup(tag),
															 NULL,
															 (void (*)(void *))list_free),
							0 };
						_lou_logMessage(LOU_LOG_DEBUG, "Query has feature '%s:%s'",
								f1.feature.key, v);
						_lou_logMessage(LOU_LOG_DEBUG, "Query has feature '%s:%s'",
								f2.feature.key, v);
						features = list_conj(features,
								memcpy(malloc(sizeof(f1)), &f1, sizeof(f1)), NULL, NULL,
								(void (*)(void *))feat_free);
						features = list_conj(features,
								memcpy(malloc(sizeof(f2)), &f2, sizeof(f2)), NULL, NULL,
								(void (*)(void *))feat_free);
					} else {
						FeatureWithImportance f = {
							feat_new(k, tag, NULL, (void (*)(void *))list_free), 0
						};
						_lou_logMessage(LOU_LOG_DEBUG, "Query has feature '%s:%s'", k, v);
						features = list_conj(features,
								memcpy(malloc(sizeof(f)), &f, sizeof(f)), NULL, NULL,
								(void (*)(void *))feat_free);
					}
				} else {
					FeatureWithImportance f = { feat_new(k, v, (void *(*)(void *))strdup,
														(void (*)(void *))free),
						0 };
					_lou_logMessage(LOU_LOG_DEBUG, "Query has feature '%s:%s'", k, v);
					features =
							list_conj(features, memcpy(malloc(sizeof(f)), &f, sizeof(f)),
									NULL, NULL, (void (*)(void *))feat_free);
					if (strcasecmp(k, "unicode-range") == 0) unicodeRange = 1;
				}
				free(k);
				free(v);
				key = val = NULL;
				keySize = valSize = 0;
			}
			if (*c == '\0') break;
		} else if (*c == ':') {
			if (!key || val)
				goto compile_error;
			else {
				c = &query[pos++];
				if (isValidChar(*c)) {
					val = c;
					valSize = 1;
				} else
					goto compile_error;
			}
		} else if (isValidChar(*c)) {
			if (val)
				valSize++;
			else if (key)
				keySize++;
			else {
				key = c;
				keySize = 1;
			}
		} else
			goto compile_error;
	}
	// add defaults
	if (!unicodeRange) {
		// default value of unicode-range is determined by CHARSIZE
		static char value[5] = "";
		if (!*value) sprintf(value, "ucs%ld", CHARSIZE);
		FeatureWithImportance *f = memcpy(malloc(sizeof(FeatureWithImportance)),
				(&(FeatureWithImportance){
						feat_new("unicode-range", value, (void *(*)(void *))strdup,
								(void (*)(void *))free),
						-1 }),
				sizeof(FeatureWithImportance));
		_lou_logMessage(LOU_LOG_DEBUG, "Query has feature '%s:%s'", f->feature.key,
				f->feature.val);
		features = list_conj(features, f, NULL, NULL, (void (*)(void *))feat_free);
	}
	// attach importance to features
	{
		int k = 1;
		List *l;
		for (l = features; l; l = l->tail) {
			FeatureWithImportance *f = l->head;
			f->importance = k++;
		}
	}
	// sort features by key (alphabetical order)
	return list_sort(features, (int (*)(void *, void *))cmpKeys);
compile_error:
	_lou_logMessage(LOU_LOG_ERROR, "Unexpected character '%c' at position %d", *c, pos);
	list_free(features);
	return NULL;
}

/**
 * Convert a widechar string to a normal string.
 */
static char *
widestrToStr(const widechar *str, size_t n) {
	char *result = malloc((1 + n) * sizeof(char));
	size_t k;
	for (k = 0; k < n; k++) result[k] = (char)str[k];
	result[k] = '\0';
	return result;
}

/**
 * Extract a list of features from a table. The features are of type
 * FeatureWithLineNumber.
 */
static List *
analyzeTable(const char *table, int activeOnly) {
	static char fileName[MAXSTRING];
	List *features = NULL;
	FileInfo info;

	{
		char **resolved = _lou_resolveTable(table, NULL);

		if (resolved == NULL) {
			_lou_logMessage(LOU_LOG_ERROR, "Cannot resolve table '%s'", table);
			return NULL;
		}

		sprintf(fileName, "%s", *resolved);
		int k = 0;

		for (k = 0; resolved[k]; k += 1) free(resolved[k]);
		free(resolved);

		if (k > 1) {
			_lou_logMessage(
					LOU_LOG_ERROR, "Table '%s' resolves to more than one file", table);
			return NULL;
		}
	}

	info.fileName = fileName;
	info.encoding = noEncoding;
	info.status = 0;
	info.lineNumber = 0;
	if ((info.in = fopen(info.fileName, "rb"))) {
		FeatureWithLineNumber *region = NULL;
		FeatureWithLineNumber *language = NULL;
		int unicodeRange = 0;
		while (_lou_getALine(&info)) {
			if (info.linelen == 0)
				;
			else if (info.line[0] == '#') {
				if (info.linelen >= 2 &&
						(info.line[1] == '+' ||
								(!activeOnly && info.line[1] == '-' &&
										!(info.linelen > 2 && info.line[2] == '-')))) {
					int active = (info.line[1] == '+');
					widechar *key = NULL;
					widechar *val = NULL;
					size_t keySize = 0;
					size_t valSize = 0;
					info.linepos = 2;
					if (info.linepos < info.linelen &&
							isValidChar((char)info.line[info.linepos])) {
						key = &info.line[info.linepos];
						keySize = 1;
						info.linepos++;
						while (info.linepos < info.linelen &&
								isValidChar((char)info.line[info.linepos])) {
							keySize++;
							info.linepos++;
						}
						char *k = widestrToStr(key, keySize);
						int isLangTag = isLanguageTag(k, keySize);
						if (info.linepos < info.linelen &&
								info.line[info.linepos] == ':') {
							info.linepos++;
							while (info.linepos < info.linelen &&
									isSpace((char)info.line[info.linepos]))
								info.linepos++;
							if (info.linepos < info.linelen &&
									(!active ||
											isValidChar((char)info.line[info.linepos]) ||
											(isLangTag &&
													'*' == info.line[info.linepos]))) {
								val = &info.line[info.linepos];
								valSize = 1;
								info.linepos++;
								while (info.linepos < info.linelen &&
										(!active ||
												isValidChar(
														(char)info.line[info.linepos]))) {
									valSize++;
									info.linepos++;
								}
							} else {
								free(k);
								goto compile_error;
							}
						}
						if (info.linepos == info.linelen) {
							char *v = val ? widestrToStr(val, valSize) : NULL;
							if (!v) {
								free(k);
								goto compile_error;
							}
							if (!active) {
								// normalize space
								int i = 0;
								int j = 0;
								int space = 1;
								while (v[i]) {
									if (isSpace(v[i])) {
										if (!space) {
											v[j++] = ' ';
											space = 1;
										}
									} else {
										v[j++] = v[i];
										space = 0;
									}
									i++;
								}
								if (j > 0 && v[j - 1] == ' ') j--;
								v[j] = '\0';
							}
							if (isLangTag) {
								List *tag = parseLanguageTag(v);
								if (!tag) {
									_lou_logMessage(LOU_LOG_ERROR,
											"Not a valid language tag: %s (line %d)", v,
											info.lineNumber);
									list_free(features);
									return NULL;
								}
								if (strcasecmp(k, "locale") == 0) {
									FeatureWithLineNumber *f1 = memcpy(
											malloc(sizeof(FeatureWithLineNumber)),
											(&(FeatureWithLineNumber){
													feat_new("language", tag, NULL,
															(void (*)(void *))list_free),
													info.lineNumber }),
											sizeof(FeatureWithLineNumber));
									FeatureWithLineNumber *f2 = memcpy(
											malloc(sizeof(FeatureWithLineNumber)),
											(&(FeatureWithLineNumber){
													feat_new("region", list_dup(tag),
															NULL,
															(void (*)(void *))list_free),
													info.lineNumber }),
											sizeof(FeatureWithLineNumber));
									_lou_logMessage(LOU_LOG_DEBUG,
											"Table has feature '%s:%s'", f1->feature.key,
											v);
									_lou_logMessage(LOU_LOG_DEBUG,
											"Table has feature '%s:%s'", f2->feature.key,
											v);
									features = list_conj(features, f1, NULL, NULL,
											(void (*)(void *))feat_free);
									features = list_conj(features, f2, NULL, NULL,
											(void (*)(void *))feat_free);
									if (!language) language = f1;
									if (!region) region = f2;
								} else {
									FeatureWithLineNumber *f = memcpy(
											malloc(sizeof(FeatureWithLineNumber)),
											(&(FeatureWithLineNumber){
													feat_new(k, tag, NULL,
															(void (*)(void *))list_free),
													info.lineNumber }),
											sizeof(FeatureWithLineNumber));
									_lou_logMessage(LOU_LOG_DEBUG,
											"Table has feature '%s:%s'", k, v);
									features = list_conj(features, f, NULL, NULL,
											(void (*)(void *))feat_free);
									if (strcasecmp(k, "language") == 0) {
										if (!language) language = f;
									} else if (strcasecmp(k, "region") == 0) {
										if (!region) region = f;
									}
								}
							} else {
								FeatureWithLineNumber *f = memcpy(
										malloc(sizeof(FeatureWithLineNumber)),
										(&(FeatureWithLineNumber){
												feat_new(k, v, (void *(*)(void *))strdup,
														(void (*)(void *))free),
												info.lineNumber }),
										sizeof(FeatureWithLineNumber));
								_lou_logMessage(
										LOU_LOG_DEBUG, "Table has feature '%s:%s'", k, v);
								features = list_conj(features, f, NULL, NULL,
										(void (*)(void *))feat_free);
								if (strcasecmp(k, "unicode-range") == 0) unicodeRange = 1;
							}
							free(k);
							free(v);
						} else {
							free(k);
							goto compile_error;
						}
					} else
						goto compile_error;
				}
			} else
				break;
		}
		fclose(info.in);
		// add defaults
		if (!region && language) {
			region = memcpy(malloc(sizeof(FeatureWithLineNumber)),
					(&(FeatureWithLineNumber){
							feat_new("region", list_dup(language->feature.val), NULL,
									(void (*)(void *))list_free),
							-1 }),
					sizeof(FeatureWithLineNumber));
			char *v = serializeLanguageTag(region->feature.val);
			_lou_logMessage(
					LOU_LOG_DEBUG, "Table has feature '%s:%s'", region->feature.key, v);
			free(v);
			features =
					list_conj(features, region, NULL, NULL, (void (*)(void *))feat_free);
		}
		if (features && !unicodeRange) {
			// by default we assume unicode-range: ucs2
			FeatureWithLineNumber *f = memcpy(malloc(sizeof(FeatureWithLineNumber)),
					(&(FeatureWithLineNumber){
							feat_new("unicode-range", "ucs2", (void *(*)(void *))strdup,
									(void (*)(void *))free),
							-1 }),
					sizeof(FeatureWithLineNumber));
			_lou_logMessage(LOU_LOG_DEBUG, "Table has feature '%s:%s'", f->feature.key,
					f->feature.val);
			features = list_conj(features, f, NULL, NULL, (void (*)(void *))feat_free);
		}
	} else
		_lou_logMessage(LOU_LOG_ERROR, "Cannot open table '%s'", info.fileName);
	return list_sort(features, (int (*)(void *, void *))cmpFeatures);
compile_error:
	if (info.linepos < info.linelen)
		_lou_logMessage(LOU_LOG_ERROR, "Unexpected character '%c' on line %d, column %d",
				info.line[info.linepos], info.lineNumber, info.linepos);
	else
		_lou_logMessage(LOU_LOG_ERROR, "Unexpected newline on line %d", info.lineNumber);
	list_free(features);
	return NULL;
}

static List *tableIndex = NULL;

void EXPORT_CALL
lou_indexTables(const char **tables) {
	const char **table;
	list_free(tableIndex);
	tableIndex = NULL;
	for (table = tables; *table; table++) {
		_lou_logMessage(LOU_LOG_DEBUG, "Analyzing table %s", *table);
		List *features = analyzeTable(*table, 1);
		if (features) {
			TableMeta m = { strdup(*table), features };
			tableIndex = list_conj(tableIndex, memcpy(malloc(sizeof(m)), &m, sizeof(m)),
					NULL, NULL, free);
		}
	}
	if (!tableIndex) _lou_logMessage(LOU_LOG_WARN, "No tables were indexed");
}

/**
 * Returns the list of files found in a single directory.
 */
#ifdef _MSC_VER
static List *
listDir(List *list, char *dirName) {
	static char glob[MAXSTRING];
	static char fileName[MAXSTRING];
	WIN32_FIND_DATAA ffd;
	HANDLE hFind;
	sprintf(glob, "%s%c%c", dirName, DIR_SEP, '*');
	hFind = FindFirstFileA(glob, &ffd);
	if (hFind == INVALID_HANDLE_VALUE) {
		_lou_logMessage(LOU_LOG_WARN, "%s is not a directory", dirName);
	} else {
		do {
			if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				sprintf(fileName, "%s%c%s", dirName, DIR_SEP, ffd.cFileName);
				list = list_conj(list, strdup(fileName), NULL, NULL, free);
			}
		} while (FindNextFileA(hFind, &ffd));
		FindClose(hFind);
	}
	return list;
}
#else  /* !_MSC_VER */
static List *
listDir(List *list, char *dirName) {
	static char fileName[MAXSTRING];
	struct stat info;
	DIR *dir;
	struct dirent *file;
	if ((dir = opendir(dirName))) {
		while ((file = readdir(dir))) {
			sprintf(fileName, "%s%c%s", dirName, DIR_SEP, file->d_name);
			if (stat(fileName, &info) == 0 && !(info.st_mode & S_IFDIR)) {
				list = list_conj(list, strdup(fileName), NULL, NULL, free);
			}
		}
		closedir(dir);
	} else {
		_lou_logMessage(LOU_LOG_WARN, "%s is not a directory", dirName);
	}
	return list;
}
#endif /* !_MSC_VER */

/**
 * Returns the list of files found on searchPath, where searchPath is a
 * comma-separated list of directories.
 */
static List *
listFiles(char *searchPath) {
	List *list = NULL;
	char *dirName;
	int pos = 0;
	int n;
	while (1) {
		for (n = 0; searchPath[pos + n] != '\0' && searchPath[pos + n] != ','; n++)
			;
		dirName = malloc(n + 1);
		dirName[n] = '\0';
		memcpy(dirName, &searchPath[pos], n);
		list = listDir(list, dirName);
		free(dirName);
		pos += n;
		if (searchPath[pos] == '\0')
			break;
		else
			pos++;
	}
	return list;
}

static void
indexTablePath(void) {
	char *searchPath;
	List *tables;
	void *tablesArray;
	_lou_logMessage(
			LOU_LOG_WARN, "Tables have not been indexed yet. Indexing LOUIS_TABLEPATH.");
	searchPath = _lou_getTablePath();
	tables = listFiles(searchPath);
	tablesArray = list_toArray(tables, NULL);
	lou_indexTables(tablesArray);
	free(searchPath);
	list_free(tables);
	free(tablesArray);
}

char *EXPORT_CALL
lou_findTable(const char *query) {
	if (!tableIndex) indexTablePath();
	List *queryFeatures = parseQuery(query);
	int bestQuotient = 0;
	char *bestMatch = NULL;
	List *l;
	for (l = tableIndex; l; l = l->tail) {
		TableMeta *table = l->head;
		int q = matchFeatureLists(queryFeatures, table->features, 0);
		if (q > bestQuotient) {
			bestQuotient = q;
			if (bestMatch) free(bestMatch);
			bestMatch = strdup(table->name);
		}
	}
	list_free(queryFeatures);
	if (bestMatch) {
		_lou_logMessage(LOU_LOG_INFO, "Best match: %s (%d)", bestMatch, bestQuotient);
		return bestMatch;
	} else {
		_lou_logMessage(LOU_LOG_INFO, "No table could be found for query '%s'", query);
		return NULL;
	}
}

typedef struct {
	char *name;
	int matchQuotient;
} TableMatch;

static int
cmpMatches(TableMatch *m1, TableMatch *m2) {
	if (m1->matchQuotient > m2->matchQuotient)
		return -1;
	else
		return 1;
}

char **EXPORT_CALL
lou_findTables(const char *query) {
	char **tablesArray;
	List *matches = NULL;
	if (!tableIndex) indexTablePath();
	List *queryFeatures = parseQuery(query);
	List *l;
	for (l = tableIndex; l; l = l->tail) {
		TableMeta *table = l->head;
		int quotient = matchFeatureLists(queryFeatures, table->features, 0);
		if (quotient > 0) {
			TableMatch m = { strdup(table->name), quotient };
			matches = list_conj(matches, memcpy(malloc(sizeof(m)), &m, sizeof(m)),
					(int (*)(void *, void *))cmpMatches, NULL, free);
		}
	}
	list_free(queryFeatures);
	if (matches) {
		_lou_logMessage(LOU_LOG_INFO, "%d matches found", list_size(matches));
		int i = 0;
		tablesArray = malloc((1 + list_size(matches)) * sizeof(void *));
		for (List *m = matches; m; m = m->tail)
			tablesArray[i++] = ((TableMatch *)m->head)->name;
		tablesArray[i] = NULL;
		list_free(matches);
		return tablesArray;
	} else {
		_lou_logMessage(LOU_LOG_INFO, "No table could be found for query '%s'", query);
		return NULL;
	}
}

char *EXPORT_CALL
lou_getTableInfo(const char *table, const char *key) {
	char *value = NULL;
	List *features = analyzeTable(table, 0);
	List *l;
	int lineNumber = -1;  // line number of first matching feature
	for (l = features; l; l = l->tail) {
		FeatureWithLineNumber *f = l->head;
		int cmp = strcasecmp(f->feature.key, key);
		if (cmp == 0) {
			if (lineNumber < 0 || lineNumber > f->lineNumber) {
				if (isLanguageTag(key, MAXSTRING))
					value = serializeLanguageTag(f->feature.val);
				else
					value = strdup(f->feature.val);
				lineNumber = f->lineNumber;
			}
		} else if (cmp > 0) {
			break;
		}
	}
	list_free(features);
	return value;
}

char **EXPORT_CALL
lou_listTables(void) {
	void *tablesArray;
	List *tables = NULL;
	List *l;
	if (!tableIndex) indexTablePath();
	for (l = tableIndex; l; l = l->tail) {
		TableMeta *table = l->head;
		tables = list_conj(
				tables, strdup(table->name), (int (*)(void *, void *))strcmp, NULL, NULL);
	}
	tablesArray = list_toArray(tables, NULL);
	list_free(tables);
	return tablesArray;
}
