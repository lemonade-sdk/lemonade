import type { Page } from '@playwright/test';

type LoadStep = { id: string; op: string; params: Record<string, unknown> };

/** Mock the same asynchronous load-job endpoints used by the Models GUI. */
export async function mockCompletedModelLoadJobs(
  page: Page,
  onLoad: (params: Record<string, unknown>) => void,
): Promise<void> {
  const jobs = new Map<string, Record<string, unknown>>();
  let nextId = 0;

  await page.route(/\/api\/v1\/jobs(?:\/[^/?]+(?:\/interrupt)?)?(?:\?.*)?$/, async route => {
    const { pathname } = new URL(route.request().url());
    const method = route.request().method().toUpperCase();

    if (pathname.endsWith('/api/v1/jobs') && method === 'POST') {
      const body = route.request().postDataJSON() as {
        name: string;
        definition: { steps: LoadStep[] };
      };
      const steps = body.definition?.steps;
      if (!Array.isArray(steps) || !steps.length || steps.some(step => step.op !== 'load')) {
        return route.fulfill({ status: 400, json: { error: 'Expected load-job steps' } });
      }
      const id = `mock-load-${++nextId}`;
      jobs.set(id, {
        id,
        name: body.name,
        status: 'completed',
        steps: steps.map(step => ({ ...step, status: 'completed', duration_ms: 0 })),
        inputs: {},
        context: {},
        cursor: '',
        created_at: new Date().toISOString(),
      });
      for (const step of steps) onLoad(step.params);
      return route.fulfill({ status: 202, json: { id } });
    }

    const match = pathname.match(/\/api\/v1\/jobs\/([^/]+)(?:\/interrupt)?$/);
    if (!match) return route.fallback();
    const job = jobs.get(decodeURIComponent(match[1]));
    if (!job) return route.fulfill({ status: 404, json: { error: 'Unknown job' } });

    if (pathname.endsWith('/interrupt') && method === 'POST') {
      job.status = 'interrupted';
      return route.fulfill({ json: { status: 'interrupting' } });
    }
    if (method === 'GET') return route.fulfill({ json: job });
    if (method === 'DELETE') {
      jobs.delete(match[1]);
      return route.fulfill({ status: 204, body: '' });
    }
    return route.fallback();
  });
}
