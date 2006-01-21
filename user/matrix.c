#include <inc/lib.h>
#include <inc/malloc.h>

/* from demo.c */
int rand(int nseed);

static const unsigned char matrix_failure[3][36] = {
	{218, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 191, 10},
	{179, 10, ' ', 10, 'S', 10, 'Y', 10, 'S', 10, 'T', 10, 'E', 10, 'M', 10, ' ', 10, 'F', 10, 'A', 10, 'I', 10, 'L', 10, 'U', 10, 'R', 10, 'E', 10, ' ', 10, 179, 10},
	{192, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 196, 10, 217, 10}
};

#define SIZE(matrix) (80 * (matrix)->rows)
#define BUFFER(matrix) (SIZE(matrix) * 2)
#define RUNS(matrix) (24 * (matrix)->rows / 5)
#define HOTS(matrix) (8 * (matrix)->rows)

struct MATRIX {
	int status, rows;
	unsigned char * code;		/* just hold the characters */
	unsigned char * visible;	/* mask out the ones not lit */
	unsigned char * highlight;	/* highlight the ones falling */
	unsigned char * buffer;		/* text memory buffer */
	struct {
		int x, y;
	} *starts, *stops, *hots;
};

static void update_matrix(struct MATRIX * matrix)
{
	int i;
	/* change some characters */
	for(i = 0; i != 8 * matrix->rows / 5; i++)
		matrix->code[rand(0) % SIZE(matrix)] = rand(0) & 15;
	
	/* do this every other run */
	if((matrix->status = !matrix->status))
	{
		for(i = 0; i != RUNS(matrix); i++)
		{
			if(matrix->stops[i].y > -1)
				matrix->visible[matrix->stops[i].x + matrix->stops[i].y * 80] = 0;
			matrix->stops[i].y++;
			if(matrix->stops[i].y == matrix->rows)
			{
				matrix->starts[i].x = rand(0) % 80;
				matrix->starts[i].y = 0;
				matrix->stops[i].x = matrix->starts[i].x;
				matrix->stops[i].y = -2 - (rand(0) % matrix->rows) / 2;
			}
			if(matrix->starts[i].y < matrix->rows && matrix->starts[i].x != -1)
				matrix->visible[matrix->starts[i].x + matrix->starts[i].y * 80] = 1;
			matrix->starts[i].y++;
		}
	}
	
	/* update the highlights */
	for(i = 0; i != HOTS(matrix); i++)
	{
		matrix->highlight[matrix->hots[i].x + matrix->hots[i].y * 80] = 0;
		matrix->hots[i].y++;
		if(matrix->hots[i].y == matrix->rows)
		{
			matrix->hots[i].x = rand(0) % 80;
			matrix->hots[i].y = 0;
		}
		matrix->highlight[matrix->hots[i].x + matrix->hots[i].y * 80] = 16;
	}
}

static struct MATRIX * matrix_init(int rows)
{
	int i;
	struct MATRIX * matrix = malloc(sizeof(*matrix));
	
	matrix->status = 0;
	matrix->rows = rows;
	
	matrix->code = malloc(SIZE(matrix));
	matrix->visible = malloc(SIZE(matrix));
	matrix->highlight = malloc(SIZE(matrix));
	matrix->buffer = malloc(BUFFER(matrix));
	
	matrix->starts = malloc(RUNS(matrix) * sizeof(*matrix->starts));
	matrix->stops = malloc(RUNS(matrix) * sizeof(*matrix->stops));
	matrix->hots = malloc(HOTS(matrix) * sizeof(*matrix->hots));
	
	/* init the starts and stops and hotspots */
	for(i = 0; i != RUNS(matrix); i++)
	{
		matrix->stops[i].x = rand(0) % SIZE(matrix);
		matrix->stops[i].y = matrix->stops[i].x / 80;
		matrix->stops[i].x = matrix->stops[i].x % 80;
		matrix->starts[i].x = -1;
	}
	for(i = 0; i != HOTS(matrix); i++)
	{
		matrix->hots[i].x = rand(0) % SIZE(matrix);
		matrix->hots[i].y = matrix->hots[i].x / 80;
		matrix->hots[i].x = matrix->hots[i].x % 80;
	}
	
	/* fill the character buffers */
	for(i = 0; i != SIZE(matrix); i++)
	{
		matrix->code[i] = rand(0) & 15;
		matrix->visible[i] = 0;
		matrix->highlight[i] = 0;
	}
	
	return matrix;
}

static void matrix_destroy(struct MATRIX * matrix)
{
	free(matrix->hots);
	free(matrix->stops);
	free(matrix->starts);
	free(matrix->buffer);
	free(matrix->highlight);
	free(matrix->visible);
	free(matrix->code);
	free(matrix);
}

void matrix(int argc, char * argv[])
{
	int i, tmult = 5, go = -1;
	int rows = sys_vga_map_text(0xB8000);
	struct MATRIX * matrix = matrix_init(rows);
	int offset = (((rows - 3) / 2) * 80 + 31) * 2;
	
	/* get things going before starting screen output */
	for(i = 0; i != 6 * rows; i++)
		update_matrix(matrix);
	
	while(go)
	{
		if(go == -1 && getchar_nb() != -1)
			go = 200;
		
		update_matrix(matrix);
		
		for(i = 0; i != SIZE(matrix); i++)
		{
			matrix->buffer[i << 1] = (matrix->visible[i]) ? "0123456789ABCDEF"[matrix->code[i]] : 32;
			matrix->buffer[(i << 1) + 1] = matrix->highlight[i] ? 10 : 2;
		}
		
		if(go > 0)
		{
			if(go < 60 || (go / 20) & 1)
			{
				memcpy(matrix->buffer + offset, matrix_failure[0], 36);
				memcpy(matrix->buffer + offset + 160, matrix_failure[1], 36);
				memcpy(matrix->buffer + offset + 320, matrix_failure[2], 36);
			}
			if(--go < 30)
				tmult++;
		}
		
		memcpy((void *) 0xB8000, matrix->buffer, BUFFER(matrix));
		jsleep(tmult * HZ / 100);
	}
	
	memcpy(matrix->buffer + offset, matrix_failure[0], 36);
	memcpy(matrix->buffer + offset + 160, matrix_failure[1], 36);
	memcpy(matrix->buffer + offset + 320, matrix_failure[2], 36);
	memcpy((void *) 0xB8000, matrix->buffer, BUFFER(matrix));
	
	matrix_destroy(matrix);
}
