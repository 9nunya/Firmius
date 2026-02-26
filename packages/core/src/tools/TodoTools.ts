import { z } from "zod";
import type { ITool, ToolContext, TodoItem, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";

const TodoInputSchema = z.object({
  operation: z.enum(["add", "update", "remove", "list", "clear"]).describe("Operation to perform"),
  items: z.array(z.object({
    content: z.string().optional().describe("Todo content (required for add)"),
    priority: z.enum(["high", "medium", "low"]).optional().describe("Priority level (default: medium)"),
    id: z.number().optional().describe("Todo ID (required for update)"),
    status: z.enum(["pending", "in_progress", "completed"]).optional().describe("New status (for update)")
  })).optional().describe("Array of todo items. Required for add/update operations. MUST be an array, NOT a string."),
  ids: z.array(z.number()).optional().describe("Array of todo IDs to remove. Required for remove operation.")
});

export type TodoInput = z.infer<typeof TodoInputSchema>;

export interface TodoOutput {
  todos: TodoItem[];
  updated?: number;
  removed?: number;
  cleared?: number;
}

function renumberTodos(todos: TodoItem[]): TodoItem[] {
  return todos.map((todo, index) => ({
    ...todo,
    id: index + 1
  }));
}

function handleAdd(
  items: Array<{ content?: string; priority?: string; id?: number; status?: string }>,
  context: ToolContext
): ToolResult<TodoOutput> {
  const now = Date.now();
  const newTodos: TodoItem[] = [];
  
  for (const item of items) {
    if (!item.content) continue;
    const todo: TodoItem = {
      id: context.agent.state.nextTodoId,
      content: item.content,
      status: "pending",
      priority: (item.priority as "high" | "medium" | "low") ?? "medium",
      createdAt: now
    };
    newTodos.push(todo);
    context.agent.state.nextTodoId++;
  }

  context.agent.state.todos.push(...newTodos);

  return {
    success: true,
    summary: `Added ${newTodos.length} todo(s)`,
    output: { todos: context.agent.state.todos }
  };
}

function handleUpdate(
  items: Array<{ content?: string; priority?: string; id?: number; status?: string }>,
  context: ToolContext
): ToolResult<TodoOutput> {
  let updatedCount = 0;

  for (const updateItem of items) {
    if (updateItem.id === undefined) continue;
    const todoIndex = context.agent.state.todos.findIndex(t => t.id === updateItem.id);
    if (todoIndex === -1) continue;

    const todo = context.agent.state.todos[todoIndex]!;
    if (updateItem.status !== undefined) todo.status = updateItem.status as "pending" | "in_progress" | "completed";
    if (updateItem.content !== undefined) todo.content = updateItem.content;
    if (updateItem.priority !== undefined) todo.priority = updateItem.priority as "high" | "medium" | "low";
    updatedCount++;
  }

  return {
    success: true,
    summary: `Updated ${updatedCount} todo(s)`,
    output: { todos: context.agent.state.todos, updated: updatedCount }
  };
}

function handleRemove(
  ids: number[],
  context: ToolContext
): ToolResult<TodoOutput> {
  const idsToRemove = new Set(ids);
  const initialCount = context.agent.state.todos.length;
  context.agent.state.todos = context.agent.state.todos.filter(t => !idsToRemove.has(t.id));
  context.agent.state.todos = renumberTodos(context.agent.state.todos);
  context.agent.state.nextTodoId = context.agent.state.todos.length + 1;
  const removed = initialCount - context.agent.state.todos.length;

  return {
    success: true,
    summary: `Removed ${removed} todo(s)`,
    output: { todos: context.agent.state.todos, removed }
  };
}

export const TodoTool: ITool<TodoInput, TodoOutput> = {
  metadata: {
    name: "todo",
    description: `Manage your todo list.`,
    scope: ToolScope.Todo
  },
  input: TodoInputSchema,
  execute: async (input: TodoInput, context: ToolContext): Promise<ToolResult<TodoOutput>> => {
    switch (input.operation) {
      case "add":
        return handleAdd(input.items || [], context);
      case "update":
        return handleUpdate(input.items || [], context);
      case "remove":
        return handleRemove(input.ids || [], context);
      case "list":
        return { success: true, summary: `Listed ${context.agent.state.todos.length} todos`, output: { todos: context.agent.state.todos } };
      case "clear":
        const count = context.agent.state.todos.length;
        context.agent.state.todos = [];
        context.agent.state.nextTodoId = 1;
        return { success: true, summary: `Cleared ${count} todos`, output: { todos: [], cleared: count } };
      default:
        return { success: false, summary: "Invalid operation", error: "Invalid operation" };
    }
  },
  summarizeInput: (input: TodoInput) => {
    return `todo: ${input.operation}`;
  },
  summary: (output: ToolResult<TodoOutput>) => {
    return output.summary;
  }
};

export const AllTodoTools = [TodoTool];
