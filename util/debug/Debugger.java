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
	private boolean renderFree;
	private int debugRev;
	private int debugOpcodeRev;
	
	public Debugger(String name, DataInput input) throws BadInputException, IOException
	{
		super(input);
		this.name = name;
		modules = new HashMap();
		opcodes = new Vector();
		state = new SystemState();
		applied = 0;
		renderFree = false;

		debugRev = input.readInt();
		debugOpcodeRev = input.readInt();
		ensureSupportedRevision();

		addModule(new InfoModule(input));
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		addModule(new ChdescInfoModule(input));
		
		short module = input.readShort();
		if(module != 0)
			throw new UnexpectedModuleException(module);
	}
	
	private void ensureSupportedRevision()
		throws UnsupportedStreamRevisionException
	{
		/* before the days of stream revision data */
		if (debugRev == 65536 && debugOpcodeRev == 1262764639)
			throw new UnsupportedStreamRevisionException(0, 0, 1288);
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
	
	public int readOpcodes() throws BadInputException, IOException
	{
		try {
			for(;;)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		return opcodes.size();
	}
	
	public int readOpcodes(int count) throws BadInputException, IOException
	{
		try {
			while(count-- > 0)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		return opcodes.size();
	}
	
	public boolean replayAll()
	{
		boolean change = false;
		while(applied < opcodes.size())
		{
			Opcode opcode = (Opcode) opcodes.get(applied++);
			opcode.applyTo(state);
			if(opcode.hasEffect())
				change = true;
		}
		return change;
	}
	
	public boolean replay(int count)
	{
		boolean change = false;
		while(applied < opcodes.size() && count-- > 0)
		{
			Opcode opcode = (Opcode) opcodes.get(applied++);
			opcode.applyTo(state);
			if(opcode.hasEffect())
				change = true;
		}
		return change;
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
	
	public boolean getRenderFree()
	{
		return renderFree;
	}
	
	public void setRenderFree(boolean renderFree)
	{
		this.renderFree = renderFree;
	}
	
	public void render(Writer output, boolean landscape) throws IOException
	{
		String title = "";
		if(applied > 0)
			title = opcodes.get(applied - 1).toString();
		state.render(output, title, renderFree, landscape);
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
			interpreter.addCommand(new FreeCommand());
			interpreter.addCommand(new GuiCommand());
			interpreter.addCommand(new JumpCommand());
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
				dbg = (Debugger) interpreter.runCommandLine(line, null, false);
			}
			
			interpreter.runStdinCommands("debug> ", dbg, true);
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
