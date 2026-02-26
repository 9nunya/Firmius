import { describe, test, expect, beforeEach } from "bun:test";
import type { IHost } from "@firmius/shared";
import { TodoTool } from "../../tools/TodoTools";
import { createToolContext } from "../test_utils";
import type { TodoInput } from "../../tools/TodoTools";

const SUITE_NAME = "todo";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runTodoToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    describe(`TodoTools (${hostName})`, () => {
        let host: IHost;

        beforeEach(async () => {
            host = await getHost();
            await host.init();
        });

        test("add single todo", async () => {
            const ctx = createToolContext(host);

            const res = await TodoTool.execute({
                operation: "add",
                items: [{ content: "Test todo item", priority: "high" }]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.todos.length).toBe(1);
                expect(res.output.todos[0]!.content).toBe("Test todo item");
                expect(res.output.todos[0]!.priority).toBe("high");
                expect(res.output.todos[0]!.status).toBe("pending");
                expect(res.output.todos[0]!.id).toBe(1);
            }
            expect(ctx.agent.state.todos.length).toBe(1);
        });

        test("add batch todos", async () => {
            const ctx = createToolContext(host);

            const res = await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "First task", priority: "high" },
                    { content: "Second task", priority: "medium" },
                    { content: "Third task", priority: "low" }
                ]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.todos.length).toBe(3);
                expect(res.output.todos[0]!.id).toBe(1);
                expect(res.output.todos[1]!.id).toBe(2);
                expect(res.output.todos[2]!.id).toBe(3);
                expect(res.output.todos[1]!.priority).toBe("medium");
            }
            expect(ctx.agent.state.todos.length).toBe(3);
        });

        test("add with default priority", async () => {
            const ctx = createToolContext(host);

            const res = await TodoTool.execute({
                operation: "add",
                items: [{ content: "Default priority task" }]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            expect(ctx.agent.state.todos[0]!.priority).toBe("medium");
        });

        test("update single todo", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [{ content: "Task to update", priority: "medium" }]
            } as TodoInput, ctx);

            const res = await TodoTool.execute({
                operation: "update",
                items: [{ id: 1, status: "in_progress", content: "Updated task" }]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.updated).toBe(1);
            }
            expect(ctx.agent.state.todos[0]!.status).toBe("in_progress");
            expect(ctx.agent.state.todos[0]!.content).toBe("Updated task");
        });

        test("update batch todos", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Task 1", priority: "medium" },
                    { content: "Task 2", priority: "medium" },
                    { content: "Task 3", priority: "medium" }
                ]
            } as TodoInput, ctx);

            const res = await TodoTool.execute({
                operation: "update",
                items: [
                    { id: 1, status: "completed" },
                    { id: 2, status: "in_progress" },
                    { id: 3, status: "completed", priority: "low" }
                ]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.updated).toBe(3);
            }
            expect(ctx.agent.state.todos[0]!.status).toBe("completed");
            expect(ctx.agent.state.todos[1]!.status).toBe("in_progress");
            expect(ctx.agent.state.todos[2]!.status).toBe("completed");
        });

        test("remove single todo with ID shifting", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Task A", priority: "medium" },
                    { content: "Task B", priority: "medium" },
                    { content: "Task C", priority: "medium" },
                    { content: "Task D", priority: "medium" }
                ]
            } as TodoInput, ctx);

            expect(ctx.agent.state.todos.length).toBe(4);

            const res = await TodoTool.execute({
                operation: "remove",
                ids: [2]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.removed).toBe(1);
            }

            expect(ctx.agent.state.todos.length).toBe(3);
            expect(ctx.agent.state.todos[0]!.id).toBe(1);
            expect(ctx.agent.state.todos[0]!.content).toBe("Task A");
            expect(ctx.agent.state.todos[1]!.id).toBe(2);
            expect(ctx.agent.state.todos[1]!.content).toBe("Task C");
            expect(ctx.agent.state.todos[2]!.id).toBe(3);
            expect(ctx.agent.state.todos[2]!.content).toBe("Task D");
        });

        test("remove batch todos with ID shifting", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Task A", priority: "medium" },
                    { content: "Task B", priority: "medium" },
                    { content: "Task C", priority: "medium" },
                    { content: "Task D", priority: "medium" },
                    { content: "Task E", priority: "medium" }
                ]
            } as TodoInput, ctx);

            const res = await TodoTool.execute({
                operation: "remove",
                ids: [2, 4]
            } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.removed).toBe(2);
            }

            expect(ctx.agent.state.todos.length).toBe(3);
            expect(ctx.agent.state.todos.map(t => t.content)).toEqual(["Task A", "Task C", "Task E"]);
            expect(ctx.agent.state.todos.map(t => t.id)).toEqual([1, 2, 3]);
        });

        test("list todos", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Pending task", priority: "high" },
                    { content: "In progress task", priority: "medium" },
                    { content: "Completed task", priority: "low" }
                ]
            } as TodoInput, ctx);

            await TodoTool.execute({
                operation: "update",
                items: [
                    { id: 2, status: "in_progress" },
                    { id: 3, status: "completed" }
                ]
            } as TodoInput, ctx);

            const res = await TodoTool.execute({ operation: "list" } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.todos.length).toBe(3);
                expect(res.output.todos.filter(t => t.status === "pending").length).toBe(1);
                expect(res.output.todos.filter(t => t.status === "in_progress").length).toBe(1);
                expect(res.output.todos.filter(t => t.status === "completed").length).toBe(1);
            }
        });

        test("clear all todos", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Task 1", priority: "medium" },
                    { content: "Task 2", priority: "medium" },
                    { content: "Task 3", priority: "medium" }
                ]
            } as TodoInput, ctx);

            expect(ctx.agent.state.todos.length).toBe(3);

            const res = await TodoTool.execute({ operation: "clear" } as TodoInput, ctx);

            expect(res.success).toBe(true);
            if (res.output) {
                expect(res.output.cleared).toBe(3);
            }
            expect(ctx.agent.state.todos.length).toBe(0);
            expect(ctx.agent.state.nextTodoId).toBe(1);
        });

        test("ID sequence continues after operations", async () => {
            const ctx = createToolContext(host);

            await TodoTool.execute({
                operation: "add",
                items: [
                    { content: "Task 1", priority: "medium" },
                    { content: "Task 2", priority: "medium" }
                ]
            } as TodoInput, ctx);

            await TodoTool.execute({
                operation: "remove",
                ids: [1]
            } as TodoInput, ctx);

            await TodoTool.execute({
                operation: "add",
                items: [{ content: "Task 3", priority: "medium" }]
            } as TodoInput, ctx);

            expect(ctx.agent.state.todos.length).toBe(2);
            expect(ctx.agent.state.todos[0]!.id).toBe(1);
            expect(ctx.agent.state.todos[0]!.content).toBe("Task 2");
            expect(ctx.agent.state.todos[1]!.id).toBe(2);
            expect(ctx.agent.state.todos[1]!.content).toBe("Task 3");
        });

        test("summarizeInput and summary", async () => {
            const ctx = createToolContext(host);

            const addInput = { operation: "add" as const, items: [{ content: "Test", priority: "medium" as const }] };
            expect(TodoTool.summarizeInput(addInput)).toBe("todo: add");

            const updateInput = { operation: "update" as const, items: [{ id: 1, status: "completed" as const }] };
            expect(TodoTool.summarizeInput(updateInput)).toBe("todo: update");

            const removeInput = { operation: "remove" as const, ids: [1, 2, 3] };
            expect(TodoTool.summarizeInput(removeInput)).toBe("todo: remove");

            const listInput = { operation: "list" as const };
            expect(TodoTool.summarizeInput(listInput)).toBe("todo: list");

            const clearInput = { operation: "clear" as const };
            expect(TodoTool.summarizeInput(clearInput)).toBe("todo: clear");

            const addRes = await TodoTool.execute(addInput as TodoInput, ctx);
            expect(TodoTool.summary!(addRes, addInput)).toContain("Added");

            const listRes = await TodoTool.execute(listInput as TodoInput, ctx);
            expect(TodoTool.summary!(listRes, listInput)).toContain("Listed");

            const clearRes = await TodoTool.execute(clearInput as TodoInput, ctx);
            expect(TodoTool.summary!(clearRes, clearInput)).toContain("Cleared");
        });
    });
}
