import java.io.*;
import java.util.*;

import command.*;

public class Debugger extends OpcodeFactory
{
	public final String name;
	
	private HashMap modules;
	private Vector opcodes;
	private SystemState state;
	private int applied;
	
	public Debugger(String name, DataInput input) throws BadInputException, IOException
	{
		super(input);
		this.name = name;
		modules = new HashMap();
		opcodes = new Vector();
		state = new SystemState();
		applied = 0;
		
		addModule(new InfoModule(input));
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		addModule(new ChdescInfoModule(input));
		
		short module = input.readShort();
		if(module != 0)
			throw new UnexpectedModuleException(module);
	}
	
	public void addModule(Module module)
	{
		//Short key = Short.valueOf(module.getModuleNumber());
		Short key = new Short(module.getModuleNumber());
		if(modules.containsKey(key))
			throw new RuntimeException("Duplicate module registered!");
		modules.put(key, module);
	}
	
	public Opcode readOpcode() throws BadInputException, IOException
	{
		String file = readString();
		int line = input.readInt();
		String function = readString();
		
		short number = input.readShort();
		//Short key = Short.valueOf(number);
		Short key = new Short(number);
		Module module = (Module) modules.get(key);
		if(module == null)
			throw new UnexpectedModuleException(number);
		
		Opcode opcode = module.readOpcode();
		opcode.setFile(file);
		opcode.setLine(line);
		opcode.setFunction(function);
		return opcode;
	}
	
	public void readOpcodes() throws BadInputException, IOException
	{
		try {
			for(;;)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
	}
	
	public void readOpcodes(int count) throws BadInputException, IOException
	{
		try {
			while(count-- > 0)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
	}
	
	public void replayAll()
	{
		while(applied < opcodes.size())
		{
			Opcode opcode = (Opcode) opcodes.get(applied++);
			opcode.applyTo(state);
		}
	}
	
	public void replay(int count)
	{
		while(applied < opcodes.size() && count-- > 0)
		{
			Opcode opcode = (Opcode) opcodes.get(applied++);
			opcode.applyTo(state);
		}
	}
	
	public void resetState()
	{
		state = new SystemState();
		applied = 0;
	}
	
	public SystemState getState()
	{
		return state;
	}
	
	public int getApplied()
	{
		return applied;
	}
	
	public int getOpcodeCount()
	{
		return opcodes.size();
	}
	
	public Opcode getOpcode(int opcode)
	{
		return (Opcode) opcodes.get(opcode);
	}
	
	public String toString()
	{
		return "Debugging " + name + ", read " + opcodes.size() + " opcodes, applied " + applied;
	}
	
	public static void main(String args[])
	{
		if(args.length != 0 && args.length != 1 && args.length != 2)
		{
			System.err.println("Usage: java Debugger [file [count]]");
			return;
		}
		
		try {
			CommandInterpreter interpreter = new CommandInterpreter();
			Debugger dbg = null;
			
			interpreter.addCommand(new CloseCommand());
			interpreter.addCommand(new ListCommand());
			interpreter.addCommand(new LoadCommand());
			interpreter.addCommand(new RenderCommand());
			interpreter.addCommand(new ResetCommand());
			interpreter.addCommand(new RunCommand());
			interpreter.addCommand(new StatusCommand());
			interpreter.addCommand(new StepCommand());
			interpreter.addCommand(new ViewCommand());
			
			/* "built-in" commands */
			interpreter.addCommand(new HelpCommand());
			interpreter.addCommand(new QuitCommand());
			
			if(args.length != 0)
			{
				String line = "load " + args[0];
				if(args.length != 1)
					line += " " + args[1];
				dbg = (Debugger) interpreter.runCommandLine(line, null);
			}
			
			interpreter.runStdinCommands("debug> ", dbg);
		}
		catch(IOException e)
		{
			System.err.println(e);
		}
		catch(CommandException e)
		{
			System.err.println(e);
		}
	}
}
