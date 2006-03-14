import java.io.DataInput;
//import java.io.IOException;

public class InfoBdName extends Opcode
{
	private final int bd;
	private final String name;
	
	public InfoBdName(int bd, String name)
	{
		this.bd = bd;
		this.name = name;
	}
	
	public void applyTo(SystemState state)
	{
		state.setBdName(bd, name);
	}
	
	public boolean isSkippable()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_INFO_BD_NAME: bd = " + SystemState.hex(bd) + ", name = " + name;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_BD_NAME, "KDB_INFO_BD_NAME", InfoBdName.class);
		factory.addParameter("bd", 4);
		factory.addParameter("name", -1);
		return factory;
	}
}
