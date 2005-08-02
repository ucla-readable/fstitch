import java.io.DataInput;
import java.io.IOException;

public class InfoMark extends Opcode
{
	private final short module;
	
	public InfoMark(short module)
	{
		this.module = module;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_MARK, "KDB_INFO_MARK", InfoMark.class);
		factory.addParameter("module", 2);
		return factory;
	}
}
