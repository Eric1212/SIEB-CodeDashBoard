/* refresh_third_party — checker + updater des deps third_party.
 *
 * Parse third_party/manifest.toml (via tomlc17), compare les commits
 * pinnés au HEAD upstream (github API), affiche un tableau status.
 *
 * Modes manifest :
 *   "vendored"  : fichiers via raw.githubusercontent.com, listés dans files[]
 *   "tree"      : chemins amont (répertoires entiers) via clone sparse,
 *                 listés dans paths[]
 *   "submodule" : pinned via .gitmodules, refresh = git submodule update
 *
 * Champs optionnels qui déplacent le SHA cible :
 *   tracks_submodule_of = "parent:chemin"  → le SHA du submodule du parent
 *   watch = "chemin"  → le DERNIER COMMIT AYANT TOUCHÉ ce chemin sur la ref,
 *              au lieu du HEAD de la branche. Pour une dep qui ne vendorise
 *              qu'un sous-répertoire : sinon le tablier agité du reste du
 *              dépôt la fait passer pour en retard sur chaque commit, et le
 *              statut ne veut plus rien dire. Voir fetch_latest_sha_at_path.
 *
 * Usage :
 *   refresh_third_party                  # check all, table status
 *   refresh_third_party --bump <name>    # fetch latest pour <name>, rewrite manifest
 *   refresh_third_party --bump-all       # tous les bumps en un shot
 *
 * Le bump :
 *   vendored : curl chaque file de la nouvelle version, écrase ; réécriture
 *              du champ commit dans manifest.toml
 *   tree     : git init + sparse-checkout + fetch --depth 1 du SHA visé,
 *              puis mirror des chemins demandés dans third_party/<name>/
 *   submodule: cd <path> && git fetch && git checkout <new-sha> ;
 *              caller doit ensuite `git add` + commit
 *
 * Réseau : popen("curl ...") pour l'API github, popen("git ...") pour le
 * mode tree — pas de dépendance libcurl.
 */
#define _POSIX_C_SOURCE 200809L

#include "tomlc17.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MANIFEST_PATH "third_party/manifest.toml"
#define STAMP_PATH    ".git/refresh_third_party.stamp"
#define CACHE_PATH    ".git/refresh_third_party.cache.toml"
#define AUTO_THROTTLE_SECONDS (1 * 3600)   /* 24h */
#define MAX_DEPS      32

/* Extrait owner/repo depuis une URL github : https://github.com/X/Y → "X/Y" */
static int parse_owner_repo(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "github.com/");
    if (!p) return -1;
    p += strlen("github.com/");
    size_t n = strlen(p);
    if (n >= cap) return -1;
    memcpy(out, p, n + 1);
    /* trim trailing .git */
    if (n > 4 && strcmp(out + n - 4, ".git") == 0) out[n - 4] = '\0';
    return 0;
}

/* Extrait le premier "sha":"..." d'une réponse de l'API github. Pas de parsing
   JSON complet : sur les deux endpoints utilisés ici, le premier sha de la
   réponse EST le commit demandé — objet unique pour /commits/<ref>, et pour
   /commits?sha=&path= le tableau commence par le commit le plus récent, dont
   "sha" est le premier champ de l'objet. Vérifié sur les réponses réelles. */
static int first_sha_of(const char *url, char out[41]) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -sS '%s' 2>/dev/null", url);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[8192];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);
    char *p = strstr(buf, "\"sha\"");
    if (!p) return -1;
    p = strchr(p + 5, '"');   /* opening quote du value */
    if (!p) return -1;
    p++;                      /* premier char du SHA */
    for (int i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)p[i])) return -1;
        out[i] = p[i];
    }
    out[40] = '\0';
    return 0;
}

/* Lit le SHA HEAD d'une ref via l'API github. Stocke 40 chars + NUL. */
static int fetch_latest_sha(const char *owner_repo, const char *ref, char out[41]) {
    char url[512];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/commits/%s",
             owner_repo, ref);
    return first_sha_of(url, out);
}

/* Lit le SHA du DERNIER COMMIT AYANT TOUCHÉ un chemin, sur une ref.
   Ce qu'il faut à une dep qui ne vendorise qu'un sous-répertoire du dépôt :
   le HEAD de la branche avance sur tout le reste (modèles.dev synchronise ses
   catalogues de fournisseurs plusieurs fois par jour), et une dep qui le suit
   se déclare éternellement en retard pour un contenu qui n'a pas bougé — un
   signal qui force à ouvrir le diff pour découvrir qu'il est vide. Suivre le
   chemin rend le statut vrai : BEHIND veut alors dire « il y a du nouveau ici ».

   Le mirror matériel n'y change rien : les commits entre le dernier ayant
   touché le chemin et le HEAD de la branche ne touchent QUE d'autres chemins,
   donc l'arbre du chemin suivi est le même des deux côtés. Ce qui change est
   la assertion du manifest : « ce pin est le dernier état de CE que nous
   copions », et non « nous étions là dans la branche ». */
static int fetch_latest_sha_at_path(const char *owner_repo, const char *ref,
                                    const char *path, char out[41]) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/commits?sha=%s&path=%s&per_page=1",
             owner_repo, ref, path);
    return first_sha_of(url, out);
}

/* Sk8-post : Récupère le SHA d'un submodule (mode=160000) à un path donné
   dans l'arbre git d'un commit, via /repos/owner/git/trees/<commit>.
   Utilisé pour locker dear-imgui sur le SHA imgui submodule de cimgui@pin
   sans risquer de bumper dear-imgui de manière désynchronisée. */
static int fetch_submodule_sha(const char *owner_repo, const char *commit,
                                const char *submodule_path, char out[41]) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "curl -sS 'https://api.github.com/repos/%s/git/trees/%s' 2>/dev/null",
             owner_repo, commit);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    /* Trees moyens-gros : 128 KB couvre confortablement les repos de
       quelques milliers de fichiers (cimgui ~ 50 entries, dear-imgui ~ 20). */
    static char buf[131072];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);
    /* Cherche l'entry avec path == submodule_path. Format JSON GitHub
       est formaté avec espaces et newlines, e.g.
         "path": "imgui",
         "mode": "160000",
         "type": "commit",
         "sha": "abc..."
       On itère sur chaque "path" et on compare la valeur de manière
       tolérante au whitespace. */
    size_t sub_len = strlen(submodule_path);
    char *p = buf;
    while ((p = strstr(p, "\"path\"")) != NULL) {
        char *q = p + 6;
        while (*q == ' ' || *q == '\t' || *q == ':') q++;
        if (*q != '"') { p++; continue; }
        q++;
        if (strncmp(q, submodule_path, sub_len) != 0 || q[sub_len] != '"') {
            p++; continue;
        }
        /* Match — cherche "sha" dans le même objet (avant '}'). */
        char *obj_end = strchr(q, '}');
        if (!obj_end) return -1;
        char *sha_field = strstr(q, "\"sha\"");
        if (!sha_field || sha_field > obj_end) return -1;
        sha_field += 5;
        while (*sha_field == ' ' || *sha_field == '\t' || *sha_field == ':') sha_field++;
        if (*sha_field != '"') return -1;
        sha_field++;
        for (int i = 0; i < 40; i++) {
            if (!isxdigit((unsigned char)sha_field[i])) return -1;
            out[i] = sha_field[i];
        }
        out[40] = '\0';
        return 0;
    }
    return -1;
}

/* Récupère le nombre de commits entre `base` et `head` via l'endpoint
   /compare. On lit le champ "behind_by" (combien on est en retard).
   Retourne >=0 le count, ou -1 si la requête/parse échoue. */
static int fetch_commit_distance(const char *owner_repo,
                                  const char *base, const char *head) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "curl -sS 'https://api.github.com/repos/%s/compare/%s...%s' 2>/dev/null",
             owner_repo, base, head);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    /* 32 KB : la réponse /compare contient base_commit + merge_base_commit
       détaillés AVANT ahead_by, donc 8K ne suffit pas pour gros écarts. */
    static char buf[32768];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = fread(buf + total, 1, sizeof(buf) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);
    /* /compare donne ahead_by (head - base) et behind_by (base - head).
       Nous on veut "combien de commits en avant a l'upstream", soit le
       ahead_by quand base=pin, head=upstream. */
    char *p = strstr(buf, "\"ahead_by\"");
    if (!p) return -1;
    p = strchr(p + 10, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    int count = 0;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) {
        count = count * 10 + (*p - '0');
        p++;
    }
    return count;
}

/* Sépare "src:dest[:mode]" en strings + flag exécutable.
   mode optionnel : "exec" → chmod 0755, sinon défaut 0644.
   Modifie file_spec en place (utilise un buffer static). */
static void split_file_spec(const char *spec, const char **src,
                             const char **dest, bool *is_exec) {
    static char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", spec);
    *is_exec = false;
    char *p = strchr(tmp, ':');
    if (!p) { *src = *dest = tmp; return; }
    *p = '\0'; *src = tmp;
    char *rest = p + 1;
    char *p2 = strchr(rest, ':');
    if (p2) {
        *p2 = '\0';
        *dest = rest;
        if (strcmp(p2 + 1, "exec") == 0) *is_exec = true;
    } else {
        *dest = rest;
    }
}

/* Télécharge un fichier raw github vers third_party/<name>/<dest>. */
static int fetch_raw_file(const char *owner_repo, const char *commit,
                          const char *src_path, const char *local_path) {
    /* Pire cas annoncé par -Wformat-truncation : local_path (1023) + les
       trois segments amont + le gabarit curl ~= 1770 octets. 1024 tronquait
       en silence la commande, donc sur un chemin long le curl partait chercher
       une URL cassée. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -sS -fL --create-dirs -o '%s' "
             "'https://raw.githubusercontent.com/%s/%s/%s'",
             local_path, owner_repo, commit, src_path);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}

/* Réécrit le champ commit d'une entrée dans manifest.toml.
   Cherche [name] puis remplace la première ligne `commit = "..."`. */
static int update_manifest_commit(const char *name, const char *new_sha) {
    FILE *in = fopen(MANIFEST_PATH, "r");
    if (!in) return -1;
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MANIFEST_PATH);
    FILE *out = fopen(tmp_path, "w");
    if (!out) { fclose(in); return -1; }

    char line[2048];
    bool in_section = false;
    bool replaced = false;
    char section_header[128];
    snprintf(section_header, sizeof(section_header), "[%s]", name);

    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '[') {
            in_section = (strncmp(line, section_header, strlen(section_header)) == 0
                          && (line[strlen(section_header)] == '\n'
                              || line[strlen(section_header)] == '\r'
                              || line[strlen(section_header)] == '\0'));
        }
        if (in_section && !replaced && strstr(line, "commit") && strchr(line, '=')) {
            /* Préserve l'indentation et l'alignement */
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "commit", 6) == 0) {
                fprintf(out, "commit   = \"%s\"\n", new_sha);
                replaced = true;
                continue;
            }
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (!replaced) { remove(tmp_path); return -1; }
    return rename(tmp_path, MANIFEST_PATH);
}

typedef struct {
    const char *name;
    const char *mode;
    const char *upstream;
    const char *ref;
    const char *commit;
    const char *license;
    /* Sk8-post : si présent, format "parent_name:submodule_path" — le SHA
       cible n'est pas master HEAD du propre upstream mais le SHA du
       submodule au path donné **dans le HEAD upstream du parent_name**.
       Permet de locker dear-imgui sur le SHA submodule de cimgui :
       quand cimgui upstream avance, dear-imgui devient BEHIND avec le
       nouveau imgui submodule comme target ; au bump-all, cimgui d'abord
       puis dear-imgui suit (les deux atterrissent cohérents).
       NULL = mode classique (master HEAD de upstream). */
    const char *tracks_submodule_of;
    /* Chemin amont à poursuivre, au lieu du HEAD de la ref : le SHA cible
       devient le dernier commit AYANT TOUCHÉ ce chemin. Une dep qui ne
       vendorise qu'un sous-répertoire du dépôt a besoin de ça, sinon elle
       se déclare en retard sur chaque commit sans rapport avec son
       périmètre. NULL = HEAD de la ref (comportement historique). */
    const char *watch;
    toml_datum_t files;   /* mode "vendored" : TOML_ARRAY de "src:dest[:exec]" */
    toml_datum_t paths;   /* mode "tree"     : TOML_ARRAY de chemins amont    */
} dep_t;

static int dep_from_toml(const char *name, toml_datum_t tab, dep_t *out) {
    out->name                 = name;
    out->mode                 = toml_seek(tab, "mode").u.s;
    out->upstream             = toml_seek(tab, "upstream").u.s;
    out->ref                  = toml_seek(tab, "ref").u.s;
    out->commit               = toml_seek(tab, "commit").u.s;
    out->license              = toml_seek(tab, "license").u.s;
    out->tracks_submodule_of  = toml_seek(tab, "tracks_submodule_of").u.s;
    out->watch                = toml_seek(tab, "watch").u.s;
    out->files                = toml_seek(tab, "files");
    out->paths                = toml_seek(tab, "paths");
    return (out->mode && out->upstream && out->ref && out->commit) ? 0 : -1;
}

/* Résout le SHA cible d'une dep. Trois cas, par priorité :
   - tracks_submodule_of : le SHA du submodule au path donné dans le HEAD
     upstream du parent (voir le Sk8-post dans dep_t) ;
   - watch : le dernier commit AYANT TOUCHÉ ce chemin sur la ref, et non le
     HEAD de la branche — ce qu'il faut à une dep qui ne vendorise qu'un
     sous-répertoire (voir fetch_latest_sha_at_path) ;
   - sinon : le HEAD de la ref. */
static int resolve_latest_sha(const dep_t *dep, const dep_t *all_deps, int n_all,
                                char out[41]) {
    if (dep->tracks_submodule_of && dep->tracks_submodule_of[0]) {
        const char *colon = strchr(dep->tracks_submodule_of, ':');
        if (!colon) return -1;
        char parent_name[64];
        size_t plen = (size_t)(colon - dep->tracks_submodule_of);
        if (plen == 0 || plen >= sizeof(parent_name)) return -1;
        memcpy(parent_name, dep->tracks_submodule_of, plen);
        parent_name[plen] = '\0';
        const char *submodule_path = colon + 1;
        if (!*submodule_path) return -1;
        const dep_t *parent = NULL;
        for (int i = 0; i < n_all; i++) {
            if (strcmp(all_deps[i].name, parent_name) == 0) { parent = &all_deps[i]; break; }
        }
        if (!parent) return -1;
        char parent_repo[128];
        if (parse_owner_repo(parent->upstream, parent_repo, sizeof(parent_repo)) != 0) return -1;
        /* Étape 1 : trouver le HEAD du parent (master / main). */
        char parent_head[41];
        if (fetch_latest_sha(parent_repo, parent->ref, parent_head) != 0) return -1;
        /* Étape 2 : query le submodule à ce HEAD. */
        return fetch_submodule_sha(parent_repo, parent_head, submodule_path, out);
    }
    char owner_repo[128];
    if (parse_owner_repo(dep->upstream, owner_repo, sizeof(owner_repo)) != 0) return -1;
    if (dep->watch && dep->watch[0])
        return fetch_latest_sha_at_path(owner_repo, dep->ref, dep->watch, out);
    return fetch_latest_sha(owner_repo, dep->ref, out);
}

/* Écrit le cache `name → latest_sha` au format TOML. La timestamp globale
   est fournie séparément (stat sur stamp file après écriture).
   Format aligné avec ce que tomlc17 sait reparser. */
static void cache_write(const dep_t *deps, int n, const char latest[][41]) {
    FILE *f = fopen(CACHE_PATH, "w");
    if (!f) return;   /* Échec silencieux : .git/ absent ? */
    fprintf(f, "# refresh_third_party — snapshot des derniers SHAs upstream\n");
    fprintf(f, "# Régénéré à chaque check réel. Voir aussi STAMP_PATH pour le timestamp.\n\n");
    fprintf(f, "[latest]\n");
    for (int i = 0; i < n; i++) {
        if (latest[i][0] != '\0') {
            fprintf(f, "%s = \"%s\"\n", deps[i].name, latest[i]);
        }
    }
    fclose(f);
}

/* Lit le cache. Pour chaque dep, peuple latest[i] (40 chars + NUL) si présent,
   sinon laisse vide. Renvoie le mtime du cache (0 si absent). */
static time_t cache_read(const dep_t *deps, int n, char latest[][41]) {
    for (int i = 0; i < n; i++) latest[i][0] = '\0';
    struct stat st;
    if (stat(CACHE_PATH, &st) != 0) return 0;
    toml_result_t res = toml_parse_file_ex(CACHE_PATH);
    if (!res.ok) return 0;
    toml_datum_t lat = toml_seek(res.toptab, "latest");
    if (lat.type == TOML_TABLE) {
        for (int i = 0; i < n; i++) {
            toml_datum_t v = toml_seek(lat, deps[i].name);
            if (v.type == TOML_STRING && v.u.str.len == 40) {
                memcpy(latest[i], v.u.s, 40);
                latest[i][40] = '\0';
            }
        }
    }
    toml_free(res);
    return st.st_mtime;
}

/* Imprime le tableau avec le header donné. latest[] peut contenir des
   chaînes vides pour signaler "inconnu" (cache partiel). Quand un dep
   est BEHIND, fait un appel /compare pour montrer la distance commits. */
static int print_table(const dep_t *deps, int n, const char latest[][41],
                       const char *header) {
    printf("%s\n", header);
    printf("%-14s %-10s %-10s %-10s %s\n",
           "NAME", "MODE", "PINNED", "LATEST", "STATUS");
    printf("------------------------------------------------------------\n");
    int behind = 0;
    int total_commits = 0;
    for (int i = 0; i < n; i++) {
        if (latest[i][0] == '\0') {
            printf("%-14s %-10s %.10s %-10s %s\n", deps[i].name, deps[i].mode,
                   deps[i].commit, "?", "unknown");
            continue;
        }
        bool diff = strncmp(deps[i].commit, latest[i], 40) != 0;
        if (diff) {
            char owner_repo[128];
            int dist = -1;
            if (parse_owner_repo(deps[i].upstream, owner_repo, sizeof(owner_repo)) == 0) {
                dist = fetch_commit_distance(owner_repo, deps[i].commit, latest[i]);
            }
            if (dist > 0) {
                char status[32];
                snprintf(status, sizeof(status), "BEHIND (%d commit%s)",
                         dist, dist == 1 ? "" : "s");
                printf("%-14s %-10s %.10s %.10s %s\n",
                       deps[i].name, deps[i].mode, deps[i].commit, latest[i], status);
                total_commits += dist;
            } else {
                printf("%-14s %-10s %.10s %.10s %s\n",
                       deps[i].name, deps[i].mode, deps[i].commit, latest[i], "BEHIND");
            }
            behind++;
        } else {
            printf("%-14s %-10s %.10s %.10s %s\n",
                   deps[i].name, deps[i].mode, deps[i].commit, latest[i], "ok");
        }
    }
    printf("------------------------------------------------------------\n");
    if (total_commits > 0) {
        printf("%d dep%s behind (%d commit%s total).\n",
               behind, behind == 1 ? "" : "s",
               total_commits, total_commits == 1 ? "" : "s");
    } else {
        printf("%d dep%s behind.\n", behind, behind == 1 ? "" : "s");
    }
    if (behind > 0) {
        printf("Bump : refresh_third_party --bump <name>  (ou --bump-all)\n");
    }
    return 0;
}

/* Check live : hit API pour chaque dep, écrit cache + imprime tableau "fresh". */
static int do_check(const dep_t *deps, int n) {
    char latest[MAX_DEPS][41];
    for (int i = 0; i < n; i++) latest[i][0] = '\0';
    int errs = 0;
    for (int i = 0; i < n; i++) {
        if (resolve_latest_sha(&deps[i], deps, n, latest[i]) != 0) {
            latest[i][0] = '\0';   /* signal "unknown" pour la table */
            errs++;
        }
    }
    cache_write(deps, n, latest);
    print_table(deps, n, latest, "third_party: fresh check");
    return errs == 0 ? 0 : 0;   /* don't fail commit on transient API issues */
}

/* Check from cache : lit le cache, imprime "cached, Xh ago". Bootstrap : si
   cache absent, fallback sur do_check live. */
static int do_check_cached(const dep_t *deps, int n) {
    char latest[MAX_DEPS][41];
    time_t cache_time = cache_read(deps, n, latest);
    if (cache_time == 0) {
        /* Pas de cache → bootstrap : check live. */
        return do_check(deps, n);
    }
    time_t age = time(NULL) - cache_time;
    char header[128];
    snprintf(header, sizeof(header),
             "third_party: cached (last API check %ldh ago, < 24h throttle)",
             (long)(age / 3600));
    print_table(deps, n, latest, header);
    return 0;
}


/* Tout mode qui TELECHARGE du contenu depuis upstream. "submodule" reste en
   dehors : son refresh est un `git submodule update` manuel. */
static bool mode_downloads(const char *mode) {
    return strcmp(mode, "vendored") == 0 || strcmp(mode, "tree") == 0;
}

/* --- mode "tree" -------------------------------------------------------
   Materialise des chemins entiers d'upstream (repertoires comme fichiers)
   dans third_party/<name>/ par un clone sparse a profondeur 1, verrouille sur
   le SHA demande. Sequence : git init, remote add, sparse-checkout set en mode
   non-cone avec un motif ANCRE par chemin demande (le chemin barre oblique,
   puis tout son contenu recursif), fetch --depth 1 de ce SHA, checkout de
   FETCH_HEAD.

   Pourquoi ce mode existe. files[] est un inventaire ecrit a la main : pour
   quatre fichiers choisis dans un repo (stb_image.h, sokol_gfx.h) c'est la
   liberte de choisir, mais pour une arborescence de 364 fichiers c'est une
   liste qui vieillit mal — un modele paru amont reste invisible, le hook
   telecharge sagement la liste au lieu de l'arbre, et rien ne le dit. Avec
   paths[], c'est l'arbre qui fait foi.

   L'ancrage est le point sensible : le motif --no-cone part de la RACINE du
   depot, et c'est ce qui borne. Un motif d'enveloppe — asterisque, puis
   « /models/ », puis asterisque — ne borne pas : l'asterisque de tete
   traverse les barres obliques, providers/X/models/Y.toml passe au travers,
   et 7 883 fichiers se materialisent au lieu de 363. C'est la faute commise
   ici meme, deux fois, avant que ce mode n'existe. */
#define TREE_TMP_PREFIX  "third_party/.tree-tmp-"
#define MAX_TREE_PATHS   16

/* Compte les fichiers reels d'un arbre (le manifest ne dit plus rien : c'est
   l'amont qui decide, donc le nombre se mesure apres coup, pas avant). */
static long count_files(const char *root) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "find '%s' -type f 2>/dev/null | wc -l", root);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    long n = -1;
    if (fscanf(fp, "%ld", &n) != 1) n = -1;
    if (pclose(fp) != 0 && n < 0) return -1;
    return n;
}

static int do_vendor_tree(const dep_t *dep, const char *sha) {
    char tmp[256], dest[256], cmd[4096], pat[2048];
    int rc = -1;

    if (dep->paths.type != TOML_ARRAY || dep->paths.u.arr.size == 0) {
        fprintf(stderr, "[%s] mode tree : paths[] manquant\n", dep->name);
        return -1;
    }
    if (dep->paths.u.arr.size > MAX_TREE_PATHS) {
        fprintf(stderr, "[%s] mode tree : paths[] trop long (%d > %d)\n",
                dep->name, dep->paths.u.arr.size, MAX_TREE_PATHS);
        return -1;
    }
    snprintf(tmp,  sizeof(tmp),  "%s%s", TREE_TMP_PREFIX, dep->name);
    snprintf(dest, sizeof(dest), "third_party/%s", dep->name);

    /* L'arbre de travail vit DANS le repo : le mirror final est un cp, et un
       repertoire sous /tmp sur un autre filesystem le paierait en copie de
       fichier par fichier, disque contre disque. */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) goto out;

    /* Motifs : par entree, le chemin ancre a la racine, puis ce meme chemin
       suivi de tout son contenu recursif. L'un des deux ne matcher rien selon
       que le nom designe un fichier ou un repertoire, et git s'en moque — ce
       qui evite d'avoir a deviner, donc de se tromper une fois sur deux. */
    size_t off = 0;
    pat[0] = '\0';
    for (int i = 0; i < dep->paths.u.arr.size; i++) {
        const char *p = dep->paths.u.arr.elem[i].u.s;
        if (!p || !*p) continue;
        int k = snprintf(pat + off, sizeof(pat) - off, " '/%s' '/%s/**'", p, p);
        if (k < 0 || (size_t)k >= sizeof(pat) - off) {
            fprintf(stderr, "[%s] mode tree : motifs trop longs\n", dep->name);
            goto out;
        }
        off += (size_t)k;
    }

    printf("[%s] clone sparse de %s\n", dep->name, dep->upstream);
    snprintf(cmd, sizeof(cmd), "git init -q '%s'", tmp);
    if (system(cmd) != 0) { fprintf(stderr, "[%s] git init a echoue\n", dep->name); goto out; }
    snprintf(cmd, sizeof(cmd), "git -C '%s' remote add origin '%s'",
             tmp, dep->upstream);
    if (system(cmd) != 0) { fprintf(stderr, "[%s] remote add a echoue\n", dep->name); goto out; }
    snprintf(cmd, sizeof(cmd), "git -C '%s' sparse-checkout set --no-cone%s >/dev/null",
             tmp, pat);
    if (system(cmd) != 0) { fprintf(stderr, "[%s] sparse-checkout a echoue\n", dep->name); goto out; }
    snprintf(cmd, sizeof(cmd), "git -C '%s' fetch -q --depth 1 origin '%s'",
             tmp, sha);
    if (system(cmd) != 0) {
        fprintf(stderr, "[%s] fetch %.10s a echoue\n", dep->name, sha);
        goto out;
    }
    snprintf(cmd, sizeof(cmd), "git -C '%s' checkout -q FETCH_HEAD", tmp);
    if (system(cmd) != 0) { fprintf(stderr, "[%s] checkout a echoue\n", dep->name); goto out; }

    /* checkout -q reussit meme si le fetch a laisse une tete deplacee : on
       verifie le SHA materialise. Sans ce controle, un echec de fetch se
       traduirait par un arbre copie depuis n'importe quel commit. */
    snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD > '%s.sha'", tmp, tmp);
    if (system(cmd) != 0) { fprintf(stderr, "[%s] rev-parse a echoue\n", dep->name); goto out; }
    char got[64] = "";
    snprintf(cmd, sizeof(cmd), "%s.sha", tmp);
    FILE *fp = fopen(cmd, "r");
    if (fp == NULL) {
        fprintf(stderr, "[%s] SHA materialise illisible (%s)\n", dep->name, cmd);
        goto out;
    }
    if (fgets(got, sizeof(got), fp) == NULL) got[0] = '\0';
    fclose(fp);
    if (strncmp(got, sha, 40) != 0) {
        fprintf(stderr, "[%s] SHA materialise %.10s != demande %.10s — abandon\n",
                dep->name, got, sha);
        goto out;
    }

    /* Mirror chemin par chemin : on retire d'abord la copie locale, ce qui
       fait disparaitre les fichiers supprimes amont. Sans ce rm, un bump
       laisserait sur disque des modeles que plus personne ne declare. */
    for (int i = 0; i < dep->paths.u.arr.size; i++) {
        const char *p = dep->paths.u.arr.elem[i].u.s;
        if (!p || !*p) continue;
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p '%s' && d='%s/%s' && rm -rf \"$d\" && "
                 "mkdir -p \"$(dirname \"$d\")\" && cp -a '%s/%s' \"$d\"",
                 dest, dest, p, tmp, p);
        if (system(cmd) != 0) {
            fprintf(stderr, "[%s] échec mirror de '%s' (chemin absent amont ?)\n",
                    dep->name, p);
            goto out;
        }
        long nf = count_files(dest);
        if (nf >= 0) printf("  mirror %s → %s (%ld fichiers au total)\n", p, dest, nf);
    }
    rc = 0;

out:
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s.sha'", tmp, tmp);
    if (system(cmd) != 0) rc = -1;
    return rc;
}


static int do_bump_one(const dep_t *dep, const dep_t *all_deps, int n_all) {
    if (strcmp(dep->mode, "submodule") == 0) {
        printf("[%s] submodule — refresh manuel :\n", dep->name);
        printf("  cd third_party/%s && git fetch && git checkout <new-sha>\n", dep->name);
        printf("  Puis : git add + commit + refresh_third_party pour pin le nouveau sha\n");
        return 0;
    }
    if (!mode_downloads(dep->mode)) {
        fprintf(stderr, "[%s] mode inconnu : %s\n", dep->name, dep->mode);
        return -1;
    }
    char owner_repo[128];
    if (parse_owner_repo(dep->upstream, owner_repo, sizeof(owner_repo)) != 0) {
        fprintf(stderr, "[%s] upstream URL invalide\n", dep->name);
        return -1;
    }
    char latest[41];
    if (resolve_latest_sha(dep, all_deps, n_all, latest) != 0) {
        fprintf(stderr, "[%s] échec resolve latest sha\n", dep->name);
        return -1;
    }
    if (strncmp(dep->commit, latest, 40) == 0) {
        printf("[%s] déjà à jour (%.10s)\n", dep->name, latest);
        return 0;
    }
    printf("[%s] %.10s → %.10s\n", dep->name, dep->commit, latest);

    /* Les deux modes de telechargement se rejointent ici : le tree materialise
       l'arbre, le vendored telecharge l'inventaire. */
    if (strcmp(dep->mode, "tree") == 0) {
        if (do_vendor_tree(dep, latest) != 0) return -1;
    } else {
        if (dep->files.type != TOML_ARRAY) {
            fprintf(stderr, "[%s] files[] manquant\n", dep->name);
            return -1;
        }
        for (int i = 0; i < dep->files.u.arr.size; i++) {
            const char *spec = dep->files.u.arr.elem[i].u.s;
            const char *src; const char *dest; bool is_exec = false;
            split_file_spec(spec, &src, &dest, &is_exec);
            char local[1024];   /* "third_party/" + name + dest(<=511) */
            snprintf(local, sizeof(local), "third_party/%s/%s", dep->name, dest);
            printf("  fetch %s → %s%s\n", src, local, is_exec ? " [exec]" : "");
            if (fetch_raw_file(owner_repo, latest, src, local) != 0) {
                fprintf(stderr, "[%s] échec fetch %s\n", dep->name, src);
                return -1;
            }
            if (is_exec) {
                if (chmod(local, 0755) != 0) {
                    fprintf(stderr, "[%s] chmod +x %s failed\n", dep->name, local);
                    return -1;
                }
            }
        }
    }
    if (update_manifest_commit(dep->name, latest) != 0) {
        fprintf(stderr, "[%s] échec rewrite manifest\n", dep->name);
        return -1;
    }
    printf("[%s] OK — manifest mis à jour. N'oublie pas git add + commit.\n", dep->name);
    return 0;
}
/* Lit le mtime du stamp file. Renvoie 0 si absent. */
static time_t stamp_mtime(void) {
    struct stat st;
    if (stat(STAMP_PATH, &st) != 0) return 0;
    return st.st_mtime;
}

static void stamp_touch(void) {
    FILE *f = fopen(STAMP_PATH, "w");
    if (f) fclose(f);   /* Échec silencieux : .git/ peut être absent */
}

/* Mode --auto : skippe si stamp récent. Désigné pour les hooks git
   post-commit qui invoquent à chaque commit. */
static bool auto_should_skip(void) {
    time_t last = stamp_mtime();
    if (last == 0) return false;   /* jamais run → run */
    return (time(NULL) - last) < AUTO_THROTTLE_SECONDS;
}

int main(int argc, char **argv) {
    bool auto_mode = (argc == 2 && strcmp(argv[1], "--auto") == 0);
    bool throttled = auto_mode && auto_should_skip();

    toml_result_t res = toml_parse_file_ex(MANIFEST_PATH);
    if (!res.ok) {
        fprintf(stderr, "manifest parse error : %s\n", res.errmsg);
        return 1;
    }
    dep_t deps[MAX_DEPS];
    int n = 0;
    toml_datum_t root = res.toptab;
    for (int i = 0; i < root.u.tab.size && n < MAX_DEPS; i++) {
        toml_datum_t v = root.u.tab.value[i];
        if (v.type != TOML_TABLE) continue;
        if (dep_from_toml(root.u.tab.key[i], v, &deps[n]) == 0) n++;
    }

    int rc = 0;
    if (argc == 1 || auto_mode) {
        if (throttled) {
            rc = do_check_cached(deps, n);   /* table from cache, marked "cached, Xh ago" */
        } else {
            rc = do_check(deps, n);          /* live API + write cache + table "fresh" */
            stamp_touch();
        }
    } else if (argc == 3 && strcmp(argv[1], "--bump") == 0) {
        for (int i = 0; i < n; i++) {
            if (strcmp(deps[i].name, argv[2]) == 0) {
                rc = do_bump_one(&deps[i], deps, n);
                goto done;
            }
        }
        fprintf(stderr, "dep inconnue : %s\n", argv[2]);
        rc = 1;
    } else if (argc == 2 && strcmp(argv[1], "--bump-all") == 0) {
        /* Sk8-post : 2 passes pour résoudre l'ordre parent→tracker.
           Pass 1 : non-trackers (cimgui d'abord, dear-imgui sauté).
           Re-parse manifest pour récupérer les nouveaux pins.
           Pass 2 : trackers (dear-imgui suit le nouveau cimgui pin).
           Le filtre porte sur tout mode qui TELECHARGE : "vendored" (fichier
           par fichier) et "tree" (clone sparse) — sinon un bump-all passerait
           à côté d'un dep tree sans un mot. */
        for (int i = 0; i < n; i++) {
            if (!mode_downloads(deps[i].mode)) continue;
            if (deps[i].tracks_submodule_of && deps[i].tracks_submodule_of[0]) continue;
            if (do_bump_one(&deps[i], deps, n) != 0) rc = 1;
        }
        /* Re-parse manifest pour avoir les nouveaux commits parents en mémoire. */
        toml_free(res);
        res = toml_parse_file_ex(MANIFEST_PATH);
        if (!res.ok) {
            fprintf(stderr, "manifest re-parse error : %s\n", res.errmsg);
            return 1;
        }
        n = 0;
        toml_datum_t root2 = res.toptab;
        for (int i = 0; i < root2.u.tab.size && n < MAX_DEPS; i++) {
            toml_datum_t v = root2.u.tab.value[i];
            if (v.type != TOML_TABLE) continue;
            if (dep_from_toml(root2.u.tab.key[i], v, &deps[n]) == 0) n++;
        }
        for (int i = 0; i < n; i++) {
            if (!mode_downloads(deps[i].mode)) continue;
            if (!deps[i].tracks_submodule_of || !deps[i].tracks_submodule_of[0]) continue;
            if (do_bump_one(&deps[i], deps, n) != 0) rc = 1;
        }
    } else {        fprintf(stderr,
                "Usage: %s              # check status (manuel)\n"
                "       %s --auto       # check si stamp > 24h (hook post-commit)\n"
                "       %s --bump NAME  # update one\n"
                "       %s --bump-all   # update all vendored\n",
                argv[0], argv[0], argv[0], argv[0]);
        rc = 2;
    }
done:
    toml_free(res);
    return rc;
}
