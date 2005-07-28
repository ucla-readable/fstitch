import java.io.DataInput;
import java.io.IOException;

public class ChdescWeakRetain extends Opcode
{
	public ChdescWeakRetain(int chdesc, int location)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_WEAK_RETAIN, "KDB_CHDESC_WEAK_RETAIN", ChdescWeakRetain.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("location", 4);
		return factory;
	}
}
